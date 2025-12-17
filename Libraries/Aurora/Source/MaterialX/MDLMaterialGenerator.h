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

#if ENABLE_MATERIALX && ENABLE_MDL

#include "../MaterialBase.h"

#include <MaterialXCore/Material.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXGenMdl/MdlShaderGenerator.h>
#include <MaterialXGenShader/DefaultColorManagementSystem.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Library.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

#include <mi/mdl_sdk.h>

BEGIN_AURORA

class MdlSdk;

namespace MaterialXCodeGen
{

class MDLMaterialGenerator
{
public:
    MDLMaterialGenerator(const shared_ptr<MdlSdk>& mdlSdk, const string& mtlxFolder, IRenderer* renderer);

    /// Generate shader code for material.
    MaterialDefinitionPtr generate(const string& document);

    /// Returns the index for the given unit name or -1 if not found.
    int indexForUnit(const string& unit) const
    {
        auto it = _unitIndices.find(unit);
        return it != _unitIndices.end() ? _unitIndices.at(unit) : -1;
    }

private:
    struct MDLGeneratorResult
    {
        string mtlxMaterialName;
        string generatedMdlCode;
        string generatedMdlName;
    };

    /// Generate MDL code for the given MaterialX document.
    bool generateMdlCode(const string& mtlxDocument, const string& mtlxMaterialName,
        MDLGeneratorResult& inoutResult) const;

    /// Compiles the MDL material in either instance or class compilation mode.
    /// The MDL module that contains the material is returned in @outModule.
    /// Returns nullptr if failed.
    mi::neuraylib::ICompiled_material* compileMdlMaterial(
        const MDLGeneratorResult& mdlGenResult, bool classCompilation,
        mi::base::Handle<const mi::neuraylib::IModule>& outModule);

    /// Generates HLSL target code for a compiled MDL material.
    /// The index of the argument block needed for constructing the argument block
    /// is returned in @outArgumentBlockIndex 
    /// Returns nullptr if failed.
    const mi::neuraylib::ITarget_code* generateTargetCode(
        const mi::neuraylib::ICompiled_material* compiledMaterial, mi::Size& outArgumentBlockIndex);

    /// Creates the default textures for a material.
    void createDefaultMaterialTextures(const mi::neuraylib::ITarget_code* targetCode,
        MDLMaterialDataPtr& mdlData, MaterialDefaultValues& defaultValues);

    shared_ptr<MdlSdk> _pMdlSdk;
    MaterialX::DocumentPtr _mtlxStdLib;
    MaterialX::FilePathVec _mtlxLibraryFolders;
    MaterialX::FileSearchPath _mtlxSearchPath;
    MaterialX::UnitConverterRegistryPtr _mtlxUnitRegistry;
    unordered_map<string, int> _unitIndices;

    IRenderer* _pRenderer;
    unordered_map<const char*, pair<string, weak_ptr<IImage>>> _imageCache;

    using TargetCodeHandle = mi::base::Handle<const mi::neuraylib::ITarget_code>;
    unordered_map<string, pair<TargetCodeHandle, mi::Size>> _cachedTargetCodes;
};

} // namespace MaterialXCodeGen

END_AURORA

#endif
