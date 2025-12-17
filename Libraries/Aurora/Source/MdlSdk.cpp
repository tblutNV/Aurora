// Copyright 2025 Autodesk, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "MdlSdk.h"

namespace
{

void* gLibMdlSdkHandle; // Pointer to the dynamic lib handle. Cached here for unload().

#ifdef MI_PLATFORM_WINDOWS
void logWindowsError()
{
    char buffer[256];
    string message = "unknown failure";
    DWORD errorCode = ::GetLastError();
    if (::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            0, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer, 255, 0))
    {
        message = buffer;
    }
    AU_ERROR("Windows error (%u): %s", errorCode, message.c_str());
}
#endif

mi::neuraylib::INeuray* loadAndGetNeuray()
{
    const char* filename = "libmdl_sdk" MI_BASE_DLL_FILE_EXT;

#ifdef MI_PLATFORM_WINDOWS
    HMODULE handle = ::LoadLibraryA(filename);
    if (!handle)
    {
        logWindowsError();
        return 0;
    }
    void* symbol = GetProcAddress(handle, "mi_factory");
    if (!symbol)
    {
        logWindowsError();
        return 0;
    }
#else  // MI_PLATFORM_WINDOWS
    void* handle = dlopen(filename, RTLD_LAZY);
    if (!handle)
    {
        AU_ERROR("%s\n", dlerror());
        return 0;
    }
    void* symbol = dlsym(handle, "mi_factory");
    if (!symbol)
    {
        AU_ERROR("%s\n", dlerror());
        return 0;
    }
#endif // MI_PLATFORM_WINDOWS
    gLibMdlSdkHandle = handle;

    mi::neuraylib::INeuray* neuray = mi::neuraylib::mi_factory<mi::neuraylib::INeuray>(symbol);
    if (!neuray)
    {
        mi::base::Handle<mi::neuraylib::IVersion> version(
            mi::neuraylib::mi_factory<mi::neuraylib::IVersion>(symbol));
        if (!version)
            AU_ERROR("Error: Incompatible library.");
        else
            AU_ERROR("Error: Library version %s does not match header version %s.",
                version->get_product_version(), MI_NEURAYLIB_PRODUCT_VERSION_STRING);
        return 0;
    }
    return neuray;
}

bool unloadNeuray()
{
#ifdef MI_PLATFORM_WINDOWS
    BOOL result = ::FreeLibrary(static_cast<HMODULE>(gLibMdlSdkHandle));
    if (!result)
    {
        logWindowsError();
        return false;
    }
#else
    int result = dlclose(gLibMdlSdkHandle);
    if (result != 0)
    {
        AU_ERROR("%s\n", dlerror());
        return false;
    }
#endif
    return true;
}

mi::Sint32 loadPlugin(mi::neuraylib::INeuray* neuray, const string& pluginName)
{
    mi::base::Handle<mi::neuraylib::IPlugin_configuration> plugin_conf(
        neuray->get_api_component<mi::neuraylib::IPlugin_configuration>());

    // try to load the requested plugin before adding any special handling
    const string path = pluginName + MI_BASE_DLL_FILE_EXT;
    mi::Sint32 res    = plugin_conf->load_plugin_library(path.c_str());
    if (res == 0)
    {
        return 0;
    }

    // return the failure code
    AU_ERROR("Failed to load the plugin library '%s'", path.c_str());
    return res;
}

} // namespace


BEGIN_AURORA

MdlSdk::MdlSdk()
{
    // load neuray
    _neuray = loadAndGetNeuray();
    if (!_neuray.is_valid_interface())
    {
        AU_ERROR("Failed to load the MDL SDK");
        return;
    }

    mi::base::Handle<const mi::neuraylib::IVersion> version(
        _neuray->get_api_component<const mi::neuraylib::IVersion>());
    AU_INFO("Loaded MDL SDK library version: %s", version->get_string());

    // add the MaterialX MDL modules to the search path
    _mdlConfig = _neuray->get_api_component<mi::neuraylib::IMdl_configuration>();
    string mtlxMdlFolder = Foundation::getModulePath() + "MaterialX/libraries/mdl";
    _mdlConfig->add_mdl_path(mtlxMdlFolder.c_str());

    // load plugins
    const string plugins[] = { "nv_openimageio", "dds" };
    for (const string& plugin : plugins)
    {
        if (loadPlugin(_neuray.get(), plugin) != 0)
        {
            AU_ERROR("Failed to load the '%s' plugin", plugin.c_str());
            return;
        }
        AU_INFO("Loaded '%s' plugin", plugin.c_str());
    }

    // start the mdl sdk
    mi::Sint32 result = _neuray->start();
    if (result != 0)
    {
        AU_ERROR("Failed to start the MDL SDK with return code: %d", result);
        return;
    }

    _database   = _neuray->get_api_component<mi::neuraylib::IDatabase>();
    _mdlFactory = _neuray->get_api_component<mi::neuraylib::IMdl_factory>();
    _imageApi   = _neuray->get_api_component<mi::neuraylib::IImage_api>();

    mi::base::Handle<mi::neuraylib::IScope> scope(_database->get_global_scope());
    _transaction = scope->create_transaction();

    mi::base::Handle<mi::neuraylib::IMdl_backend_api> mdlBackendApi(
        _neuray->get_api_component<mi::neuraylib::IMdl_backend_api>());
    _hlslBackend = mdlBackendApi->get_backend(mi::neuraylib::IMdl_backend_api::MB_HLSL);

    // configure HLSL backend
    _hlslBackend->set_option("num_texture_results", "16");
    _hlslBackend->set_option("num_texture_spaces", "1");
    _hlslBackend->set_option("material_state_struct_name", "ShadingData");
    _hlslBackend->set_option("enable_auxiliary", "on");
}

MdlSdk::~MdlSdk()
{
    _hlslBackend.reset();
    _mdlFactory.reset();
    _transaction->commit();
    _transaction.reset();
    _database.reset();
    _mdlConfig.reset();
    _imageApi.reset();

    if (_neuray->shutdown())
    {
        AU_ERROR("Failed to shutdown the MDL SDK");
    }
    _neuray.reset();

    if (!unloadNeuray())
    {
        AU_ERROR("Failed to unload the MDL SDK");
    }
}

END_AURORA
