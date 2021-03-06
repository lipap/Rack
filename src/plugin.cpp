#include <stdio.h>

#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h> // for MAXPATHLEN
#include <fcntl.h>

#include <zip.h>
#include <jansson.h>

#if ARCH_WIN
	#include <windows.h>
	#include <direct.h>
	#define mkdir(_dir, _perms) _mkdir(_dir)
#else
	#include <dlfcn.h>
#endif
#include <dirent.h>

#include "plugin.hpp"
#include "util/request.hpp"


namespace rack {

std::list<Plugin*> gPlugins;
std::string gToken;

static bool isDownloading = false;
static float downloadProgress = 0.0;
static std::string downloadName;
static std::string loginStatus;


Plugin::~Plugin() {
	for (Model *model : models) {
		delete model;
	}
}


static int loadPlugin(std::string slug) {
	#if ARCH_LIN
		std::string path = "./plugins/" + slug + "/plugin.so";
	#elif ARCH_WIN
		std::string path = "./plugins/" + slug + "/plugin.dll";
	#elif ARCH_MAC
		std::string path = "./plugins/" + slug + "/plugin.dylib";
	#endif

	// Load dynamic/shared library
	#if ARCH_WIN
		HINSTANCE handle = LoadLibrary(path.c_str());
		if (!handle) {
			int error = GetLastError();
			fprintf(stderr, "Failed to load library %s: %d\n", path.c_str(), error);
			return -1;
		}
	#elif ARCH_LIN || ARCH_MAC
		void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
		if (!handle) {
			fprintf(stderr, "Failed to load library %s: %s\n", path.c_str(), dlerror());
			return -1;
		}
	#endif

	// Call plugin init() function
	typedef Plugin *(*InitCallback)();
	InitCallback initCallback;
	#if ARCH_WIN
		initCallback = (InitCallback) GetProcAddress(handle, "init");
	#elif ARCH_LIN || ARCH_MAC
		initCallback = (InitCallback) dlsym(handle, "init");
	#endif
	if (!initCallback) {
		fprintf(stderr, "Failed to read init() symbol in %s\n", path.c_str());
		return -2;
	}

	// Add plugin to map
	Plugin *plugin = initCallback();
	if (!plugin) {
		fprintf(stderr, "Library %s did not return a plugin\n", path.c_str());
		return -3;
	}
	gPlugins.push_back(plugin);
	fprintf(stderr, "Loaded plugin %s\n", path.c_str());
	return 0;
}

void pluginInit() {


	// Load core
	// This function is defined in core.cpp
	Plugin *corePlugin = init();
	gPlugins.push_back(corePlugin);

	// Search for plugin libraries
	DIR *dir = opendir("plugins");
	if (dir) {
		struct dirent *d;
		while ((d = readdir(dir))) {
			if (d->d_name[0] == '.')
				continue;
			loadPlugin(d->d_name);
		}
		closedir(dir);
	}
}

void pluginDestroy() {
	for (Plugin *plugin : gPlugins) {
		// TODO free shared library handle with `dlclose` or `FreeLibrary`
		delete plugin;
	}
	gPlugins.clear();
}

////////////////////
// CURL and libzip helpers
////////////////////

static int extractZipHandle(zip_t *za, const char *dir) {
	int err = 0;
	for (int i = 0; i < zip_get_num_entries(za, 0); i++) {
		zip_stat_t zs;
		err = zip_stat_index(za, i, 0, &zs);
		if (err)
			return err;
		int nameLen = strlen(zs.name);

		char path[MAXPATHLEN];
		snprintf(path, sizeof(path), "%s/%s", dir, zs.name);

		if (zs.name[nameLen - 1] == '/') {
			err = mkdir(path, 0755);
			if (err)
				return err;
		}
		else {
			zip_file_t *zf = zip_fopen_index(za, i, 0);
			if (!zf)
				return 1;

			FILE *outFile = fopen(path, "wb");
			if (!outFile)
				continue;

			while (1) {
				char buffer[4096];
				int len = zip_fread(zf, buffer, sizeof(buffer));
				if (len <= 0)
					break;
				fwrite(buffer, 1, len, outFile);
			}

			err = zip_fclose(zf);
			if (err)
				return err;
			fclose(outFile);
		}
	}
	return 0;
}

static int extractZip(const char *filename, const char *dir) {
	int err = 0;
	zip_t *za = zip_open(filename, 0, &err);
	if (!za)
		return 1;

	if (!err) {
		err = extractZipHandle(za, dir);
	}

	zip_close(za);
	return err;
}

////////////////////
// plugin manager
////////////////////

static std::string apiHost = "http://api.vcvrack.com";

void pluginLogIn(std::string email, std::string password) {
	json_t *reqJ = json_object();
	json_object_set(reqJ, "email", json_string(email.c_str()));
	json_object_set(reqJ, "password", json_string(password.c_str()));
	json_t *resJ = requestJson(POST_METHOD, apiHost + "/token", reqJ);
	json_decref(reqJ);

	if (resJ) {
		json_t *errorJ = json_object_get(resJ, "error");
		if (errorJ) {
			const char *errorStr = json_string_value(errorJ);
			loginStatus = errorStr;
		}
		else {
			json_t *tokenJ = json_object_get(resJ, "token");
			if (tokenJ) {
				const char *tokenStr = json_string_value(tokenJ);
				gToken = tokenStr;
				loginStatus = "";
			}
		}
		json_decref(resJ);
	}
}

void pluginLogOut() {
	gToken = "";
}

static void pluginRefreshPlugin(json_t *pluginJ) {
	json_t *slugJ = json_object_get(pluginJ, "slug");
	if (!slugJ) return;
	const char *slug = json_string_value(slugJ);

	json_t *nameJ = json_object_get(pluginJ, "name");
	if (!nameJ) return;
	const char *name = json_string_value(nameJ);

	json_t *downloadJ = json_object_get(pluginJ, "download");
	if (!downloadJ) return;
	const char *download = json_string_value(downloadJ);

	// Find slug in plugins list
	for (Plugin *p : gPlugins) {
		if (p->slug == slug) {
			return;
		}
	}

	// If plugin is not loaded, download the zip file to /plugins
	downloadName = name;
	downloadProgress = 0.0;

	// Download zip
	std::string filename = "plugins/";
	filename += slug;
	filename += ".zip";
	bool success = requestDownload(download, filename, &downloadProgress);
	if (success) {
		// Unzip file
		int err = extractZip(filename.c_str(), "plugins");
		if (!err) {
			// Load plugin
			loadPlugin(slug);
			// Delete zip
			remove(filename.c_str());
		}
	}

	downloadName = "";
}

void pluginRefresh() {
	if (gToken.empty())
		return;

	isDownloading = true;
	downloadProgress = 0.0;
	downloadName = "";

	json_t *reqJ = json_object();
	json_object_set(reqJ, "token", json_string(gToken.c_str()));
	json_t *resJ = requestJson(GET_METHOD, apiHost + "/purchases", reqJ);
	json_decref(reqJ);

	if (resJ) {
		json_t *errorJ = json_object_get(resJ, "error");
		if (errorJ) {
			const char *errorStr = json_string_value(errorJ);
			fprintf(stderr, "Plugin refresh error: %s\n", errorStr);
		}
		else {
			json_t *purchasesJ = json_object_get(resJ, "purchases");
			size_t index;
			json_t *purchaseJ;
			json_array_foreach(purchasesJ, index, purchaseJ) {
				pluginRefreshPlugin(purchaseJ);
			}
		}
		json_decref(resJ);
	}

	isDownloading = false;
}

void pluginCancelDownload() {
	// TODO
}

bool pluginIsLoggedIn() {
	return gToken != "";
}

bool pluginIsDownloading() {
	return isDownloading;
}

float pluginGetDownloadProgress() {
	return downloadProgress;
}

std::string pluginGetDownloadName() {
	return downloadName;
}

std::string pluginGetLoginStatus() {
	return loginStatus;
}


} // namespace rack
