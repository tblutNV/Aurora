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
#pragma once

#include "pch.h"

#include <mi/mdl_sdk.h>

BEGIN_AURORA

class MdlSdk
{
public:
    MdlSdk();
    ~MdlSdk();

    // Note that these accessors don't follow the MDL SDK API design of increasing
    // the reference counters before returning the raw pointers. This class is expected
    // to own the objects.
    mi::neuraylib::INeuray* neuray() const { return _neuray.get(); }
    mi::neuraylib::IDatabase* database() const { return _database.get(); }
    mi::neuraylib::ITransaction* transaction() const { return _transaction.get(); }
    mi::neuraylib::IMdl_factory* mdlFactory() const { return _mdlFactory.get(); }
    mi::neuraylib::IMdl_backend* backend() const { return _hlslBackend.get(); }
    mi::neuraylib::IMdl_configuration* mdlConfig() const { return _mdlConfig.get(); }
    mi::neuraylib::IImage_api* imageApi() const { return _imageApi.get(); }

private:
    mi::base::Handle<mi::neuraylib::INeuray> _neuray;
    mi::base::Handle<mi::neuraylib::IDatabase> _database;
    mi::base::Handle<mi::neuraylib::ITransaction> _transaction;
    mi::base::Handle<mi::neuraylib::IMdl_factory> _mdlFactory;
    mi::base::Handle<mi::neuraylib::IMdl_backend> _hlslBackend;
    mi::base::Handle<mi::neuraylib::IMdl_configuration> _mdlConfig;
    mi::base::Handle<mi::neuraylib::IImage_api> _imageApi;
};

MAKE_AURORA_PTR(MdlSdk);

END_AURORA
