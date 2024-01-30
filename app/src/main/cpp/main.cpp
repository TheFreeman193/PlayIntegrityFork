#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "zygisk.hpp"
#include "json.hpp"
#include "dobby.h"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PIF/Native", __VA_ARGS__)

#define DEX_FILE_PATH "/data/adb/modules/playintegrityfix/classes.dex"

#define JSON_FILE_PATH "/data/adb/modules/playintegrityfix/pif.json"
#define CUSTOM_JSON_FILE_PATH "/data/adb/modules/playintegrityfix/custom.pif.json"

static int VERBOSE_LOGS = 0;

static std::map<std::string, std::string> jsonProps;

static std::string unameRelease;

typedef void (*T_Callback)(void *, const char *, const char *, uint32_t);

static std::map<void *, T_Callback> callbacks;

static std::string thisProc;
static bool isGms;

static void modify_callback(void *cookie, const char *name, const char *value, uint32_t serial) {
    if (cookie == nullptr || name == nullptr || value == nullptr || !callbacks.contains(cookie)) return;

    const char *oldValue = value;

    std::string prop(name);

    // Spoof specific property values
    if (prop == "init.svc.adbd") {
        value = "stopped";
    } else if (prop == "sys.usb.state") {
        value = "mtp";
    }

    if (jsonProps.count(prop)) {
        // Exact property match
        value = jsonProps[prop].c_str();
    } else {
        // Leading * wildcard property match
        for (const auto &p: jsonProps) {
            if (p.first.starts_with("*") && prop.ends_with(p.first.substr(1))) {
                value = p.second.c_str();
                break;
            }
        }
    }

    if (oldValue == value) {
        if (VERBOSE_LOGS > 99) LOGD("%s: [%s]: %s (unchanged)", thisProc.c_str(), name, oldValue);
    } else {
        if (VERBOSE_LOGS > 2) LOGD("%s: [%s]: %s -> %s", thisProc.c_str(), name, oldValue, value);
    }

    return callbacks[cookie](cookie, name, value, serial);
}

static void (*o_system_property_read_callback)(const prop_info *, T_Callback, void *);

static void my_system_property_read_callback(const prop_info *pi, T_Callback callback, void *cookie) {
    if (pi == nullptr || callback == nullptr || cookie == nullptr) {
        return o_system_property_read_callback(pi, callback, cookie);
    }
    callbacks[cookie] = callback;
    return o_system_property_read_callback(pi, modify_callback, cookie);
}

static int (*o_uname_callback)(struct utsname *);

static int my_uname_callback(struct utsname *buf) {
    auto ret = o_uname_callback(buf);

    if (buf && ret == 0) {
        const char *oldValue = buf->release;

        if (unameRelease.empty() || strcmp(oldValue, unameRelease.c_str()) == 0) {
            if (VERBOSE_LOGS > 99) LOGD("%s: [uname_release]: %s (unchanged)", thisProc.c_str(), oldValue);
        } else if (unameRelease.size() < SYS_NMLN) {
            if (VERBOSE_LOGS > 2) LOGD("%s: [uname_release]: %s -> %s", thisProc.c_str(), oldValue, unameRelease.c_str());
            strncpy(buf->release, unameRelease.c_str(), unameRelease.size());
        }
    }

    return ret;
}

static const std::string hardUname = "4.4.310-constant";
static int (*o_const_uname_callback)(struct utsname *);
static int const_uname_callback(struct utsname *buf) {
    auto ret = o_const_uname_callback(buf);

    if (buf && ret == 0) {
        const char *oldValue = buf->release;
        if (VERBOSE_LOGS > 2) LOGD("%s: [uname_release]: %s -> %s (constant)", thisProc.c_str(), oldValue, hardUname.c_str());
        strncpy(buf->release, hardUname.c_str(), hardUname.size());
    }

    return ret;
}

static void doPropHook() {
    void *handle = DobbySymbolResolver("libc.so", "__system_property_read_callback");
    if (handle == nullptr) {
        LOGD("%s: Couldn't find '__system_property_read_callback' handle in libc.so", thisProc.c_str());
        return;
    }
    LOGD("%s: Found libc.so '__system_property_read_callback' handle at %p", thisProc.c_str(), handle);
    DobbyHook(handle, reinterpret_cast<dobby_dummy_func_t>(my_system_property_read_callback),
        reinterpret_cast<dobby_dummy_func_t *>(&o_system_property_read_callback));
}

static void doUnameHook() {
    void *handle = DobbySymbolResolver("libc.so", "uname");
    if (handle == nullptr) {
        LOGD("%s: Couldn't find 'uname' handle in libc.so", thisProc.c_str());
        return;
    }
    LOGD("%s: Found libc.so 'uname' handle at %p", thisProc.c_str(), handle);
    DobbyHook(handle, reinterpret_cast<dobby_dummy_func_t>(my_uname_callback),
        reinterpret_cast<dobby_dummy_func_t *>(&o_uname_callback));
}

static void doConstUnameHook() {
    void *handle = DobbySymbolResolver("libc.so", "uname");
    if (handle == nullptr) {
        LOGD("%s: Couldn't find 'uname' handle in libc.so (constant)", thisProc.c_str());
        return;
    }
    LOGD("%s: Found libc.so 'uname' handle at %p (constant)", thisProc.c_str(), handle);
    DobbyHook(handle, reinterpret_cast<dobby_dummy_func_t>(const_uname_callback),
        reinterpret_cast<dobby_dummy_func_t *>(&o_const_uname_callback));
}

class PlayIntegrityFix : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        bool isGmsUnstable = false, isGmsProcess = false;

        auto rawProcess = env->GetStringUTFChars(args->nice_name, nullptr);
        auto rawDir = env->GetStringUTFChars(args->app_data_dir, nullptr);

        // Prevent crash on apps with no data dir
        if (rawDir == nullptr) {
            env->ReleaseStringUTFChars(args->nice_name, rawProcess);
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        thisProc = std::string(rawProcess);
        std::string_view dir(rawDir);

        isGms = dir.ends_with("/com.google.android.gms");
        isGmsUnstable = thisProc == "com.google.android.gms.unstable";
        isGmsProcess = isGmsUnstable ||
            thisProc == "com.google.android.gms" ||
            thisProc == "com.google.android.gms.persistent" ||
            thisProc == "com.google.android.gms.ui";

        env->ReleaseStringUTFChars(args->nice_name, rawProcess);
        env->ReleaseStringUTFChars(args->app_data_dir, rawDir);

        if (!isGms) {
            // api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // We are in GMS now, force unmount
        api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

        if (!isGmsUnstable) {
            // api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        std::vector<char> jsonVector;
        long dexSize = 0, jsonSize = 0;

        int fd = api->connectCompanion();

        read(fd, &dexSize, sizeof(long));
        read(fd, &jsonSize, sizeof(long));

        if (dexSize < 1) {
            close(fd);
            LOGD("%s: Couldn't read dex file", thisProc.c_str());
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (jsonSize < 1) {
            close(fd);
            LOGD("%s: Couldn't read json file", thisProc.c_str());
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        LOGD("%s: Read from file descriptor for 'dex' -> %ld bytes", thisProc.c_str(), dexSize);
        LOGD("%s: Read from file descriptor for 'json' -> %ld bytes", thisProc.c_str(), jsonSize);

        dexVector.resize(dexSize);
        read(fd, dexVector.data(), dexSize);

        jsonVector.resize(jsonSize);
        read(fd, jsonVector.data(), jsonSize);

        close(fd);

        std::string jsonString(jsonVector.cbegin(), jsonVector.cend());
        json = nlohmann::json::parse(jsonString, nullptr, false, true);

        jsonVector.clear();
        jsonString.clear();
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        doConstUnameHook();

        if (!isGms) return;
        if (json.empty()) return;
        readJson();
        // doUnameHook();

        if (thisProc == "com.google.android.gms.unstable") {
            if (dexVector.empty()) return;
            doPropHook();
            inject();
        }

        dexVector.clear();
        json.clear();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::vector<char> dexVector;
    nlohmann::json json;

    void readJson() {
        LOGD("%s: JSON contains %d keys!", thisProc.c_str(), static_cast<int>(json.size()));

        // Verbose logging if VERBOSE_LOGS with level number is present
        if (json.contains("VERBOSE_LOGS")) {
            if (!json["VERBOSE_LOGS"].is_null() && json["VERBOSE_LOGS"].is_string() && json["VERBOSE_LOGS"] != "") {
                VERBOSE_LOGS = stoi(json["VERBOSE_LOGS"].get<std::string>());
                if (VERBOSE_LOGS > 0) LOGD("%s: Verbose logging (level %d) enabled!", thisProc.c_str(), VERBOSE_LOGS);
            } else {
                LOGD("%s: Error parsing VERBOSE_LOGS!", thisProc.c_str());
            }
            json.erase("VERBOSE_LOGS");
        }

        // Parse kernel uname release string as a special case (neither field or property)
        if (json.contains("uname_release")) {
            if (VERBOSE_LOGS > 1) LOGD("%s: Parsing uname_release", thisProc.c_str());
            if (!json["uname_release"].is_null() && json["uname_release"].is_string() && json["uname_release"] != "") {
                unameRelease = json["uname_release"].get<std::string>();
            } else {
                LOGD("%s: Error parsing uname_release!", thisProc.c_str());
            }
            json.erase("uname_release");
        }

        std::vector<std::string> eraseKeys;
        for (auto &jsonList: json.items()) {
            if (VERBOSE_LOGS > 1) LOGD("%s: Parsing %s", thisProc.c_str(), jsonList.key().c_str());
            if (jsonList.key().find_first_of("*.") != std::string::npos) {
                // Name contains . or * (wildcard) so assume real property name
                if (!jsonList.value().is_null() && jsonList.value().is_string()) {
                    if (jsonList.value() == "") {
                        LOGD("%s: %s is empty, skipping", thisProc.c_str(), jsonList.key().c_str());
                    } else {
                        if (VERBOSE_LOGS > 0) LOGD("%s: Adding '%s' to properties list", thisProc.c_str(), jsonList.key().c_str());
                        jsonProps[jsonList.key()] = jsonList.value();
                    }
                } else {
                    LOGD("%s: Error parsing %s!", thisProc.c_str(), jsonList.key().c_str());
                }
                eraseKeys.push_back(jsonList.key());
            }
        }
        // Remove properties from parsed JSON
        for (auto key: eraseKeys) {
            if (json.contains(key)) json.erase(key);
        }
    }

    void inject() {
        LOGD("%s: JNI: Getting system classloader", thisProc.c_str());
        auto clClass = env->FindClass("java/lang/ClassLoader");
        auto getSystemClassLoader = env->GetStaticMethodID(clClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
        auto systemClassLoader = env->CallStaticObjectMethod(clClass, getSystemClassLoader);

        LOGD("%s: JNI: Creating module classloader", thisProc.c_str());
        auto dexClClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        auto dexClInit = env->GetMethodID(dexClClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        auto buffer = env->NewDirectByteBuffer(dexVector.data(), static_cast<jlong>(dexVector.size()));
        auto dexCl = env->NewObject(dexClClass, dexClInit, buffer, systemClassLoader);

        LOGD("%s: JNI: Loading module class", thisProc.c_str());
        auto loadClass = env->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        auto entryClassName = env->NewStringUTF("es.chiteroman.playintegrityfix.EntryPoint");
        auto entryClassObj = env->CallObjectMethod(dexCl, loadClass, entryClassName);

        auto entryClass = (jclass) entryClassObj;

        LOGD("%s: JNI: Sending JSON", thisProc.c_str());
        auto receiveJson = env->GetStaticMethodID(entryClass, "receiveJson", "(Ljava/lang/String;)V");
        auto javaStr = env->NewStringUTF(json.dump().c_str());
        env->CallStaticVoidMethod(entryClass, receiveJson, javaStr);

        LOGD("%s: JNI: Calling init", thisProc.c_str());
        auto entryInit = env->GetStaticMethodID(entryClass, "init", "(I)V");
        env->CallStaticVoidMethod(entryClass, entryInit, VERBOSE_LOGS);
    }
};

static void companion(int fd) {
    long dexSize = 0, jsonSize = 0;
    std::vector<char> dexVector, jsonVector;

    FILE *dex = fopen(DEX_FILE_PATH, "rb");

    if (dex) {
        fseek(dex, 0, SEEK_END);
        dexSize = ftell(dex);
        fseek(dex, 0, SEEK_SET);

        dexVector.resize(dexSize);
        fread(dexVector.data(), 1, dexSize, dex);

        fclose(dex);
    }

    FILE *json = fopen(CUSTOM_JSON_FILE_PATH, "r");
    if (!json)
        json = fopen(JSON_FILE_PATH, "r");

    if (json) {
        fseek(json, 0, SEEK_END);
        jsonSize = ftell(json);
        fseek(json, 0, SEEK_SET);

        jsonVector.resize(jsonSize);
        fread(jsonVector.data(), 1, jsonSize, json);

        fclose(json);
    }

    write(fd, &dexSize, sizeof(long));
    write(fd, &jsonSize, sizeof(long));

    write(fd, dexVector.data(), dexSize);
    write(fd, jsonVector.data(), jsonSize);

    dexVector.clear();
    jsonVector.clear();
}

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)

REGISTER_ZYGISK_COMPANION(companion)
