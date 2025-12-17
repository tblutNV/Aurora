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
#if ENABLE_MATERIALX && ENABLE_MDL

#include "MDLMaterialGenerator.h"

#include "../MdlSdk.h"
#include "../DirectX/PTImage.h"

#include <MaterialXCore/Material.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXGenMdl/MdlShaderGenerator.h>
#include <MaterialXGenShader/DefaultColorManagementSystem.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Library.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

#include <filesystem>

// Development flag to dump generated MDL code to disk.
// NOTE: This should never be enabled in committed code; it is only for local development.
#define AU_DEV_DUMP_MDL_CODE 0

namespace mx = MaterialX;

namespace
{

/// Flattens resource paths of the document to change the resource URIs into valid MDL paths.
class MdlStringResolver : public mx::StringResolver
{
private:
    MdlStringResolver(MdlSdk& mdlSdk) : _mdlSdk(mdlSdk) {}

public:
    static std::shared_ptr<MdlStringResolver> create(MdlSdk& mdlSdk)
    {
        return std::shared_ptr<MdlStringResolver>(new MdlStringResolver(mdlSdk));
    }

    void initialize(mx::DocumentPtr document)
    {
        // remove duplicates and keep order by using a set
        auto less = [](const mx::FilePath& lhs, const mx::FilePath& rhs) {
            return lhs.asString() < rhs.asString();
        };
        std::set<mx::FilePath, decltype(less)> mtlx_paths(less);
        _mtlxDocumentPaths.clear();
        _mdlSearchPaths.clear();

        // use the source search paths as base
        mx::FilePath p = mx::FilePath(document->getSourceUri()).getParentPath().getNormalized();
        mtlx_paths.insert(p);
        _mtlxDocumentPaths.append(p);

        for (auto sp : mx::getSourceSearchPath(document))
        {
            sp = sp.getNormalized();
            if (sp.exists() && mtlx_paths.insert(sp).second)
                _mtlxDocumentPaths.append(sp);
        }

        // add all search paths known to MDL
        for (size_t i = 0, n = _mdlSdk.mdlConfig()->get_mdl_paths_length(); i < n; i++)
        {
            mi::base::Handle<const mi::IString> sp_istring(_mdlSdk.mdlConfig()->get_mdl_path(i));
            p = mx::FilePath(sp_istring->get_c_str()).getNormalized();
            if (p.exists() && mtlx_paths.insert(p).second)
                _mtlxDocumentPaths.append(p);

            // keep a list of MDL search paths for resource resolution
            _mdlSearchPaths.append(p);
        }
    }

    std::string resolve(const std::string& str, const std::string&) const override
    {
        mx::FilePath normalizedPath = mx::FilePath(str).getNormalized();
        std::string resource_path;

        // in case the path is absolute we need to find a proper search path to put the file in
        if (normalizedPath.isAbsolute())
        {
            // find the highest priority search path that is a prefix of the resource path
            for (const auto& sp : _mdlSearchPaths)
            {
                if (sp.size() > normalizedPath.size())
                    continue;

                bool isParent = true;
                for (size_t i = 0; i < sp.size(); ++i)
                {
                    if (sp[i] != normalizedPath[i])
                    {
                        isParent = false;
                        break;
                    }
                }

                if (!isParent)
                    continue;

                // found a search path that is a prefix of the resource
                resource_path = normalizedPath.asString(mx::FilePath::FormatPosix)
                                    .substr(sp.asString(mx::FilePath::FormatPosix).size());
                if (resource_path[0] != '/')
                    resource_path = "/" + resource_path;
                return resource_path;
            }
        }
        else
        {
            // for relative paths we can try to find them in the MDL search paths, assuming
            // they are specified "relative" to a search path root.
            mi::base::Handle<mi::neuraylib::IMdl_entity_resolver> resolver(
                _mdlSdk.mdlConfig()->get_entity_resolver());

            resource_path = str;
            if (resource_path[0] != '/')
                resource_path = "/" + resource_path;

            mi::base::Handle<const mi::neuraylib::IMdl_resolved_resource> result(
                resolver->resolve_resource(resource_path.c_str(), nullptr, nullptr, 0, 0));

            if (result && result->get_count() > 0)
                return resource_path;
        }

        AU_ERROR(
            "MaterialX resource can not be accessed through an MDL search path. "
            "Dropping the resource from the Material. Resource Path: " +
            normalizedPath.asString());

        // drop the resource by returning the empty string.
        // alternatively, the resource could be copied into an MDL search path,
        // maybe even only temporary.
        return {};
    }

    // Get the MaterialX paths used to load the current document as well the current MDL search
    // paths in order to resolve resources by the MaterialX SDK.
    const mx::FileSearchPath& get_search_paths() const { return _mtlxDocumentPaths; }

private:
    // MDL SDK to get access to the entity resolver and the search path config.
    MdlSdk& _mdlSdk;

    // List of paths from which MaterialX can locate resources.
    // This includes the document folder and the search paths used to load the document.
    mx::FileSearchPath _mtlxDocumentPaths;

    // List of MDL search paths from which we can locate resources.
    mx::FileSearchPath _mdlSearchPaths;
};

class TargetResourceCallback
    : public mi::base::Interface_implement<mi::neuraylib::ITarget_resource_callback>
{
public:
    TargetResourceCallback(
        mi::neuraylib::ITransaction* transaction, const mi::neuraylib::ITarget_code* targetCode) :
        _transaction(transaction), _targetCode(targetCode)
    {
        // No need to increment ref counters since this object is not expected to
        // live longer than the scope in which create_argument_block is called.
    }

    mi::Uint32 get_resource_index(const mi::neuraylib::IValue_resource* resource) final
    {
        return _targetCode->get_known_resource_index(_transaction, resource);
    }

    mi::Uint32 get_string_index(const mi::neuraylib::IValue_string* s) final
    {
        mi::Size n = _targetCode->get_string_constant_count();
        for (mi::Size i = 0; i < n; ++i)
            if (strcmp(_targetCode->get_string_constant(i), s->get_value()) == 0)
                return static_cast<mi::Uint32>(i);
        return 0;
    }

private:
    mi::neuraylib::ITransaction* _transaction;
    const mi::neuraylib::ITarget_code* _targetCode;
};

const unordered_map<string, ImageFormat> pixelTypeToImageFormat = {
    { "Sint8", ImageFormat::Byte_R },
    { "Sint32", ImageFormat::Integer_R },
    { "Float32", ImageFormat::Float_R },
    { "Float32<2>", ImageFormat::Float_RG },
    { "Float32<3>", ImageFormat::Float_RGB },
    { "Float32<4>", ImageFormat::Float_RGBA },
    { "Rgba", ImageFormat::Integer_RGBA },
    { "Rgba_16", ImageFormat::Short_RGBA },
    { "Rgb_fp", ImageFormat::Float_RGB },
    { "Color", ImageFormat::Float_RGBA }
};

const unordered_map<mi::neuraylib::IValue::Kind, const char*> mdlKindToString = {
    { mi::neuraylib::IValue::VK_BOOL, "VK_BOOL" },
    { mi::neuraylib::IValue::VK_INT, "VK_INT" },
    { mi::neuraylib::IValue::VK_ENUM, "VK_ENUM" },
    { mi::neuraylib::IValue::VK_FLOAT, "VK_FLOAT" },
    { mi::neuraylib::IValue::VK_DOUBLE, "VK_DOUBLE" },
    { mi::neuraylib::IValue::VK_STRING, "VK_STRING" },
    { mi::neuraylib::IValue::VK_VECTOR, "VK_VECTOR" },
    { mi::neuraylib::IValue::VK_MATRIX, "VK_MATRIX" },
    { mi::neuraylib::IValue::VK_COLOR, "VK_COLOR" },
    { mi::neuraylib::IValue::VK_ARRAY, "VK_ARRAY" },
    { mi::neuraylib::IValue::VK_STRUCT, "VK_STRUCT" },
    { mi::neuraylib::IValue::VK_INVALID_DF, "VK_INVALID_DF" },
    { mi::neuraylib::IValue::VK_TEXTURE, "VK_TEXTURE" },
    { mi::neuraylib::IValue::VK_LIGHT_PROFILE, "VK_LIGHT_PROFILE" },
    { mi::neuraylib::IValue::VK_BSDF_MEASUREMENT, "VK_BSDF_MEASUREMENT" }
};

} // namespace

BEGIN_AURORA

namespace MaterialXCodeGen
{

MDLMaterialGenerator::MDLMaterialGenerator(
    const shared_ptr<MdlSdk>& mdlSdk, const string& mtlxFolder, IRenderer* renderer) :
    _pMdlSdk(mdlSdk), _pRenderer(renderer)
{
    // Initialize the standard library
    mx::StringSet mtlxIncludeFiles;
    _mtlxLibraryFolders.push_back("libraries");
    _mtlxSearchPath.append(mx::FilePath(mtlxFolder));

    try
    {
        _mtlxStdLib      = mx::createDocument();
        mtlxIncludeFiles = mx::loadLibraries(_mtlxLibraryFolders, _mtlxSearchPath, _mtlxStdLib);
        if (mtlxIncludeFiles.empty())
        {
            AU_ERROR("Could not find standard data libraries on the given search path: " +
                _mtlxSearchPath.asString());
        }
    }
    catch (std::exception& e)
    {
        AU_ERROR("Failed to initialize standard libraries: %s", e.what());
        return;
    }

    // Initialize unit management.
    _mtlxUnitRegistry                  = mx::UnitConverterRegistry::create();
    mx::UnitTypeDefPtr distanceTypeDef = _mtlxStdLib->getUnitTypeDef("distance");
    mx::LinearUnitConverterPtr distanceUnitConverter =
        mx::LinearUnitConverter::create(distanceTypeDef);
    _mtlxUnitRegistry->addUnitConverter(distanceTypeDef, distanceUnitConverter);
    mx::UnitTypeDefPtr angleTypeDef           = _mtlxStdLib->getUnitTypeDef("angle");
    mx::LinearUnitConverterPtr angleConverter = mx::LinearUnitConverter::create(angleTypeDef);
    _mtlxUnitRegistry->addUnitConverter(angleTypeDef, angleConverter);

    // Create the list of supported distance units.
    auto unitScales = distanceUnitConverter->getUnitScale();
    for (const auto& unitScale : unitScales)
    {
        _unitIndices[unitScale.first] = distanceUnitConverter->getUnitAsInteger(unitScale.first);
    }
}

shared_ptr<MaterialDefinition> MDLMaterialGenerator::generate(const string& document)
{
    MDLGeneratorResult mdlGenResult;
    try
    {
        if (!generateMdlCode(document, "", mdlGenResult))
        {
            AU_ERROR("Generated MDL from materialX is not valid");
            return nullptr;
        }
    }
    catch (const std::exception& e)
    {
        AU_ERROR("Generated MDL from materialX crashed: %s", e.what());
        return nullptr;
    }

    // TEMPORARY WORKAROUND: Normalize parameter names so materials with identical structure
    // but different names produce the same compiled shader hash. This reduces redundant shader
    // compilation. Remove this hack when MaterialX MDL code generation produces stable names.
    auto replaceOutsideStrings = [](const string& input, const string& from, const string& to) {
        string result;
        bool inString = false;
        size_t i = 0, lastCopy = 0;
        while (i < input.size())
        {
            if (inString && input[i] == '\\' && i + 1 < input.size()) { i += 2; continue; }
            if (input[i] == '"') { inString = !inString; i++; continue; }
            if (!inString && input.compare(i, from.size(), from) == 0)
            {
                result.append(input, lastCopy, i - lastCopy);
                result.append(to);
                i += from.size();
                lastCopy = i;
                continue;
            }
            i++;
        }
        result.append(input, lastCopy, input.size() - lastCopy);
        return result;
    };
    mdlGenResult.generatedMdlCode = replaceOutsideStrings(
        mdlGenResult.generatedMdlCode, mdlGenResult.generatedMdlName + "_", "mtlx_");
    mdlGenResult.generatedMdlCode = replaceOutsideStrings(
        mdlGenResult.generatedMdlCode, mdlGenResult.generatedMdlName.substr(2) + "_", "mtlx2_");
    

#if AU_DEV_DUMP_MDL_CODE
    string mdlFilename = mdlGenResult.generatedMdlName + ".mdl";
    if (Foundation::writeStringToFile(mdlGenResult.generatedMdlCode, mdlFilename))
        AU_INFO("Dumping MDL code to: %s", mdlFilename.c_str());
    else
        AU_WARN("Failed to write mdl code to: %s", mdlFilename.c_str());
#endif

    mi::base::Handle<const mi::neuraylib::IModule> module;
    mi::base::Handle<mi::neuraylib::ICompiled_material> compiledMaterial(
        compileMdlMaterial(mdlGenResult, /*classCompilation=*/true, module));
    if (!compiledMaterial.is_valid_interface())
        return nullptr;

    // See if we need to compile a new shader for this material
    mi::Size argBlockIndex;
    mi::base::Handle < const mi::neuraylib::ITarget_code> targetCode;
    mi::base::Uuid hash = compiledMaterial->get_hash();
    string compiledHash =
        Foundation::sFormat("%08x_%08x_%08x_%08x", hash.m_id1, hash.m_id2, hash.m_id3, hash.m_id4);

    auto it = _cachedTargetCodes.find(compiledHash);
    if (it == _cachedTargetCodes.end())
    {
        AU_INFO("Generating target code for %s with hash %s",
            mdlGenResult.generatedMdlName.c_str(), compiledHash.c_str());

        targetCode = generateTargetCode(compiledMaterial.get(), argBlockIndex);

        _cachedTargetCodes.insert({ compiledHash, { targetCode, argBlockIndex } });
    }
    else
    {
        AU_INFO("Reusing cached target code for %s with hash %s",
            mdlGenResult.generatedMdlName.c_str(), compiledHash.c_str());

        targetCode    = it->second.first;
        argBlockIndex = it->second.second;
    }

    MaterialShaderSource materialShaderSource;
    materialShaderSource.uniqueId = "mdl_" + compiledHash;
    materialShaderSource.definitions += "#include \"MDLRendererRuntime.slang\"\n";
    materialShaderSource.bsdf = targetCode->get_code();

    // Extract material resources
    MDLMaterialDataPtr mdlData = make_shared<MDLMaterialData>();

    // Collect the material textures
    MaterialDefaultValues defaultValues;

    createDefaultMaterialTextures(targetCode.get(), mdlData, defaultValues);

    // Get read-only segment and argument block data
    if (targetCode->get_ro_data_segment_count() > 0)
    {
        const char* data = targetCode->get_ro_data_segment_data(0);
        mi::Size size    = targetCode->get_ro_data_segment_size(0);
        mdlData->roDataSegment.assign(data, data + size);
    }

    if (argBlockIndex != mi::Size(-1))
    {
        TargetResourceCallback resourceCallback(_pMdlSdk->transaction(), targetCode.get());

        mi::base::Handle<const mi::neuraylib::ITarget_argument_block> argBlock(
            targetCode->create_argument_block(
                argBlockIndex, compiledMaterial.get(), &resourceCallback));

        mdlData->defaultArgumentBlockData.assign(
            argBlock->get_data(), argBlock->get_data() + argBlock->get_size());

        mi::base::Handle<const mi::neuraylib::ITarget_value_layout> layout(
            targetCode->get_argument_block_layout(argBlockIndex));
        mdlData->argumentBlockOffsets.reserve(compiledMaterial->get_parameter_count());

        for (mi::Size i = 0; i < compiledMaterial->get_parameter_count(); i++)
        {
            const char* parameterName = compiledMaterial->get_parameter_name(i);
            mi::base::Handle<const mi::neuraylib::IValue> argument(
                compiledMaterial->get_argument(i));

            PropertyValue::Type propertyType = PropertyValue::Type::Undefined;
            switch (argument->get_kind())
            {
            case mi::neuraylib::IValue::VK_BOOL:
                propertyType = PropertyValue::Type::Bool;
                break;
            case mi::neuraylib::IValue::VK_INT:
            case mi::neuraylib::IValue::VK_ENUM:
            case mi::neuraylib::IValue::VK_TEXTURE:
                propertyType = PropertyValue::Type::Int;
                break;
            case mi::neuraylib::IValue::VK_FLOAT:
                propertyType = PropertyValue::Type::Float;
                break;
            case mi::neuraylib::IValue::VK_STRING:
                propertyType = PropertyValue::Type::String;
                break;
            case mi::neuraylib::IValue::VK_VECTOR:
            {
                auto vectorValue = argument.get_interface<const mi::neuraylib::IValue_vector>();
                if (vectorValue->get_size() == 2)
                    propertyType = PropertyValue::Type::Float2;
                else if (vectorValue->get_size() == 3)
                    propertyType = PropertyValue::Type::Float3;
                else if (vectorValue->get_size() == 4)
                    propertyType = PropertyValue::Type::Float4;
                break;
            }
            case mi::neuraylib::IValue::VK_MATRIX:
            {
                auto matrixValue = argument.get_interface<const mi::neuraylib::IValue_matrix>();
                auto vectorValue = mi::base::make_handle(matrixValue->get_value(0));
                if (matrixValue->get_size() == 4 && vectorValue->get_size() == 4)
                {
                    propertyType = PropertyValue::Type::Matrix4;
                }
                break;
            }
            case mi::neuraylib::IValue::VK_COLOR:
                propertyType = PropertyValue::Type::Float3;
                break;
            case mi::neuraylib::IValue::VK_ARRAY:
            {
                auto arrayValue = argument.get_interface<const mi::neuraylib::IValue_array>();
                auto arrayType = mi::base::make_handle(arrayValue->get_type());
                auto elementType = mi::base::make_handle(arrayType->get_element_type());
                if (elementType->get_kind() == mi::neuraylib::IValue::VK_STRING)
                {
                    propertyType = PropertyValue::Type::Strings;
                }
                break;
            }
            case mi::neuraylib::IValue::VK_STRUCT:
            case mi::neuraylib::IValue::VK_DOUBLE:
            case mi::neuraylib::IValue::VK_INVALID_DF:
            case mi::neuraylib::IValue::VK_LIGHT_PROFILE:
            case mi::neuraylib::IValue::VK_BSDF_MEASUREMENT:
                // Unsupported.
                break;
            }

            if (propertyType == PropertyValue::Type::Undefined)
            {
                AU_WARN("MDL parameter '%s' with type '%s' is unsupported.", parameterName,
                    mdlKindToString.at(argument->get_kind()));
            }
 
            defaultValues.propertyDefinitions.emplace_back(
                parameterName, parameterName, propertyType);
            // defaultValues.properties is kept empty since we will use the MDL argument block as
            // the default values.

            mi::neuraylib::IValue::Kind argumentKind;
            mi::Size argumentSize;
            mi::Size dataOffset =
                layout->get_layout(argumentKind, argumentSize, layout->get_nested_state(i));
            mdlData->argumentBlockOffsets.push_back(dataOffset);
        }
    }
    
    // If cutout_opacity is constant then we can determine
    // if the material is always opaque.  
    bool isAlwaysOpaque = false;
    float opacity;
    if (compiledMaterial->get_cutout_opacity(&opacity))
    {
        isAlwaysOpaque = (opacity >= 1.0f);
    }

    function<void(MaterialBase&)> updateFunc = [isAlwaysOpaque](MaterialBase& mtl) {
        mtl.setIsOpaque(isAlwaysOpaque);
    };

    return make_shared<MaterialDefinition>(
        materialShaderSource, defaultValues, updateFunc, isAlwaysOpaque, mdlData);
}

bool MDLMaterialGenerator::generateMdlCode(const string& mtlxDocument,
    const string& mtlxMaterialName, MDLGeneratorResult& inoutResult) const
{
    // Initialize the generator context.
    mx::GenContext generatorContext = mx::MdlShaderGenerator::create();

    // Initialize search paths.
    for (const mx::FilePath& path : _mtlxSearchPath)
    {
        for (const auto folder : _mtlxLibraryFolders)
        {
            if (folder.size() > 0)
                generatorContext.registerSourceCodeSearchPath(path / folder);
        }
    }

    // Initialize color management.
    mx::DefaultColorManagementSystemPtr cms =
        mx::DefaultColorManagementSystem::create(generatorContext.getShaderGenerator().getTarget());
    cms->loadLibrary(_mtlxStdLib);
    generatorContext.getShaderGenerator().setColorManagementSystem(cms);
    generatorContext.getOptions().targetColorSpaceOverride = "lin_rec709";

    // Initialize unit management.
    mx::UnitSystemPtr unitSystem =
        mx::UnitSystem::create(generatorContext.getShaderGenerator().getTarget());
    unitSystem->loadLibrary(_mtlxStdLib);
    unitSystem->setUnitConverterRegistry(_mtlxUnitRegistry);
    generatorContext.getShaderGenerator().setUnitSystem(unitSystem);
    generatorContext.getOptions().targetDistanceUnit = "centimeter";

    // Set up read options.
    mx::XmlReadOptions readOptions;
    readOptions.readXIncludeFunction = [](mx::DocumentPtr doc, const mx::FilePath& filename,
                                           const mx::FileSearchPath& searchPath,
                                           const mx::XmlReadOptions* options) {
        mx::FilePath resolvedFilename = searchPath.find(filename);
        if (resolvedFilename.exists())
        {
            readFromXmlFile(doc, resolvedFilename, searchPath, options);
        }
        else
        {
            AU_ERROR("Include file not found: " + filename.asString());
        }
    };

    // Clear user data on the generator.
    generatorContext.clearUserData();

    // Load source document.
    mx::DocumentPtr materialDocument = mx::createDocument();
    mx::readFromXmlString(materialDocument, mtlxDocument, _mtlxSearchPath, &readOptions);

    // Import libraries.
    materialDocument->importLibrary(_mtlxStdLib);

    // flatten the resource paths of the document using a custom resolver allows
    // the change the resource URIs into valid MDL paths.
    auto customResolver = MdlStringResolver::create(*_pMdlSdk);
    customResolver->initialize(materialDocument);
    mx::flattenFilenames(materialDocument, customResolver->get_search_paths(), customResolver);

    // Validate the document.
    std::string message;
    if (!materialDocument->validate(&message))
    {
        // materialX validation failures do not mean that content can not be rendered.
        // it points to mtlx authoring errors but rendering could still be fine.
        // since MDL is robust against erroneous code we just continue. If there are problems
        // in the generated code, we detect it on module load and use a fall-back material.
        AU_WARN("Validation warnings:\n" + message);
    }

    // find (selected) renderable nodes
    mx::TypedElementPtr elementToGenerateCodeFor;
    if (!mtlxMaterialName.empty())
    {
        mx::ElementPtr elem           = materialDocument->getRoot();
        std::vector<std::string> path = Foundation::split(mtlxMaterialName, '/');
        for (size_t i = 0; i < path.size(); ++i)
        {
            elem = elem->getChild(path[i]);
            if (!elem)
                break;
        }
        // if a node is specified properly, there is only one
        if (elem)
        {
            mx::TypedElementPtr typedElem = elem ? elem->asA<mx::TypedElement>() : nullptr;
            if (typedElem)
                elementToGenerateCodeFor = typedElem;
        }
    }
    else
    {
        // find the first render-able element
        std::vector<mx::TypedElementPtr> elems;
        elems = mx::findRenderableElements(materialDocument);
        if (elems.size() > 0)
        {
            elementToGenerateCodeFor = elems[0];
        }
    }

    if (!elementToGenerateCodeFor)
    {
        if (!mtlxMaterialName.empty())
            AU_ERROR("Code generation failure: no material named '" + mtlxMaterialName + "' found");
        else
            AU_ERROR("Code generation failure: no material found");

        return false;
    }

    // Clear cached implementations, in case libraries on the file system have changed.
    generatorContext.clearNodeImplementations();

    std::string materialName = elementToGenerateCodeFor->getNamePath();
    materialName             = Foundation::replace(materialName, "/", "_");

    mx::ShaderPtr shader = nullptr;
    try
    {
        shader = generatorContext.getShaderGenerator().generate(
            materialName, elementToGenerateCodeFor, generatorContext);
    }
    catch (mx::Exception& e)
    {
        AU_ERROR("Code generation failure: %s", e.what());
        return false;
    }

    if (!shader)
    {
        AU_ERROR("Failed to generate shader for element: " + materialName);
        return false;
    }

    auto generated = shader->getSourceCode("pixel");
    if (generated.empty())
    {
        AU_ERROR("Failed to generate source code for stage.");
        return false;
    }

    inoutResult.mtlxMaterialName = materialName;
    inoutResult.generatedMdlCode =
        std::string("// generated from MaterialX using the SDK version ") +
        MaterialX::getVersionString() + "\n\n" + generated;
    inoutResult.generatedMdlName = shader->getStage("pixel").getFunctionName();
    return true;
}

mi::neuraylib::ICompiled_material* MDLMaterialGenerator::compileMdlMaterial(
    const MDLGeneratorResult& mdlGenResult, bool classCompilation,
    mi::base::Handle<const mi::neuraylib::IModule>& outModule)
{
    mi::base::Handle<mi::neuraylib::IMdl_execution_context> context(
        _pMdlSdk->mdlFactory()->create_execution_context());

    // TODO: Can any of these be enabled all the time?
    // context->set_option("fold_ternary_on_df", "on");
    // context->set_option("fold_all_bool_parameters", "on");
    // context->set_option("fold_all_enum_parameters", "on");

    // Load the generated MDL material into the database.
    // Use a hash of the MDL code to ensure unique module names for different code
    size_t codeHash = std::hash<string> {}(mdlGenResult.generatedMdlCode);
    const string moduleName =
        "::aurora::" + mdlGenResult.generatedMdlName + "_" + Foundation::sFormat("%zx", codeHash);
    mi::base::Handle<mi::neuraylib::IMdl_impexp_api> mdlImpExpApi(
        _pMdlSdk->neuray()->get_api_component<mi::neuraylib::IMdl_impexp_api>());
    mi::Sint32 result = mdlImpExpApi->load_module_from_string(_pMdlSdk->transaction(),
        moduleName.c_str(), mdlGenResult.generatedMdlCode.c_str(), context.get());
    if (result < 0)
    {
        AU_ERROR("Failed to load MDL module with error code: %d", result);
        return nullptr;
    }

    // Retrieve the module from the database.
    mi::base::Handle<const mi::IString> moduleDbName(
        _pMdlSdk->mdlFactory()->get_db_module_name(moduleName.c_str()));
    outModule =
        _pMdlSdk->transaction()->access<const mi::neuraylib::IModule>(moduleDbName->get_c_str());

    // Do function overload resolution to get the full function signature
    const string materialName = moduleName + "::" + mdlGenResult.generatedMdlName;
    mi::base::Handle<const mi::IString> materialDbName(
        _pMdlSdk->mdlFactory()->get_db_definition_name(materialName.c_str()));
    mi::base::Handle<const mi::IArray> overloads(
        outModule->get_function_overloads(materialDbName->get_c_str()));
    if (!overloads.is_valid_interface())
    {
        AU_ERROR("No overloads found for '%s'. Material name invalid.", materialDbName->get_c_str());
        return nullptr;
    }
    else if (overloads->get_length() == 0)
    {
        AU_ERROR("No overloads found for '%s'", materialDbName->get_c_str());
        return nullptr;
    }
    else if (overloads->get_length() > 1)
    {
        AU_WARN("More than one overload found for '%s'. Choosing the first.",
            materialDbName->get_c_str());
    }
    mi::base::Handle<const mi::IString> functionNameWithSignature(
        overloads->get_element<const mi::IString>(0));

    // Get the material function definition from the database.
    mi::base::Handle<const mi::neuraylib::IFunction_definition> materialDefinition(
        _pMdlSdk->transaction()->access<mi::neuraylib::IFunction_definition>(
            functionNameWithSignature->get_c_str()));
    if (!materialDefinition)
    {
        AU_ERROR("Failed to access material definition '%s'", materialDbName->get_c_str());
        return nullptr;
    }

    // Create material instance with default arguments.
    mi::base::Handle<mi::neuraylib::IFunction_call> materialInstance(
        materialDefinition->create_function_call(nullptr, &result));
    if (result != 0)
    {
        AU_ERROR("Failed to instantiate material '%s'", materialDbName->get_c_str());
        return nullptr;
    }
    mi::base::Handle<const mi::neuraylib::IMaterial_instance> materialInstance2(
        materialInstance->get_interface<mi::neuraylib::IMaterial_instance>());

    // Convert to material struct type
    mi::base::Handle<mi::neuraylib::IType_factory> tf(
        _pMdlSdk->mdlFactory()->create_type_factory(_pMdlSdk->transaction()));
    mi::base::Handle<const mi::neuraylib::IType> standardMaterialType(
        tf->get_predefined_struct(mi::neuraylib::IType_struct::SID_MATERIAL));
    context->set_option("target_type", standardMaterialType.get());

    // Compile material with instance or class compilation
    mi::Uint32 compileFlags = classCompilation
        ? mi::neuraylib::IMaterial_instance::CLASS_COMPILATION
        : mi::neuraylib::IMaterial_instance::DEFAULT_OPTIONS;

    mi::base::Handle<mi::neuraylib::ICompiled_material> compiledMaterial(
        materialInstance2->create_compiled_material(compileFlags, context.get()));

    compiledMaterial->retain();
    return compiledMaterial.get();
}

const mi::neuraylib::ITarget_code* MDLMaterialGenerator::generateTargetCode(
    const mi::neuraylib::ICompiled_material* compiledMaterial, mi::Size& outArgumentBlockIndex)
{
    mi::base::Handle<mi::neuraylib::IMdl_execution_context> context(
        _pMdlSdk->mdlFactory()->create_execution_context());

    // Create link unit
    mi::neuraylib::IMdl_backend* hlslBackend = _pMdlSdk->backend();
    mi::base::Handle<mi::neuraylib::ILink_unit> linkUnit(
        hlslBackend->create_link_unit(_pMdlSdk->transaction(), context.get()));

    // Specify which functions to generate code for
    std::vector<mi::neuraylib::Target_function_description> functionDescs;
    functionDescs.emplace_back("init", "mdl_init");
    functionDescs.emplace_back("thin_walled", "mdl_thin_walled");
    functionDescs.emplace_back("surface.scattering", "mdl_surface_scattering");
    functionDescs.emplace_back("surface.emission.emission", "mdl_surface_emission");
    functionDescs.emplace_back("surface.emission.intensity", "mdl_surface_emission_intensity");
    functionDescs.emplace_back("volume.absorption_coefficient", "mdl_volume_absorption_coefficient");

    // Add material functions to link unit
    mi::Sint32 result = linkUnit->add_material(
        compiledMaterial, functionDescs.data(), functionDescs.size(), context.get());
    if (result != 0)
    {
        AU_ERROR("Failed to add material to link unit");
        return nullptr;
    }

    // Compile cutout_opacity also as standalone version to be used in the anyhit shaders
    // to avoid costly precalculation of expressions only used by other expressions.
    result = linkUnit->add_material_expression(compiledMaterial, "geometry.cutout_opacity",
        "mdl_geometry_cutout_opacity", context.get());
    if (result != 0)
    {
        AU_ERROR("Failed to add geometry.cutout_opacity subexpression to link unit");
        return nullptr;
    }

    // Generate HLSL code for the material functions
    mi::base::Handle<const mi::neuraylib::ITarget_code> targetCode(
        hlslBackend->translate_link_unit(linkUnit.get(), context.get()));
    if (!targetCode.is_valid_interface())
    {
        AU_ERROR("Failed to translate link unit");
        return nullptr;
    }

    outArgumentBlockIndex = functionDescs[0].argument_block_index;

    targetCode->retain();
    return targetCode.get();
}

void MDLMaterialGenerator::createDefaultMaterialTextures(
    const mi::neuraylib::ITarget_code* targetCode, MDLMaterialDataPtr& mdlData,
    MaterialDefaultValues& defaultValues)
{
    // Skip the invalid texture (index == 0)
    for (mi::Size texIndex = 1; texIndex < targetCode->get_texture_count(); texIndex++)
    {
        const char* textureDbName = targetCode->get_texture(texIndex);

        string identifier;

        auto cacheIt = _imageCache.find(textureDbName);
        if (cacheIt != _imageCache.end())
        {
            identifier = cacheIt->second.first;
            mdlData->textures.push_back(cacheIt->second.second.lock());
        }
        else
        {
            mi::base::Handle<const mi::neuraylib::ITexture> texture(
                _pMdlSdk->transaction()->access<mi::neuraylib::ITexture>(textureDbName));
            mi::base::Handle<const mi::neuraylib::IImage> image(
                _pMdlSdk->transaction()->access<mi::neuraylib::IImage>(texture->get_image()));

            if (image->get_length() > 1)
            {
                AU_WARN("Image '%s' has more than one (%d) frame. Ignoring all but the first.",
                    textureDbName, (int)image->get_length());
            }
            mi::base::Handle<const mi::neuraylib::ICanvas> canvas(image->get_canvas(0, 0, 0));
            
            // Convert to linear color space if necessary and convert to supported pixel type.
            auto formatIt = pixelTypeToImageFormat.find(canvas->get_type());
            if (texture->get_effective_gamma(0, 0) != 1.0f)
            {
                string targetPixelType =
                    (formatIt == pixelTypeToImageFormat.end()) ? "Rgba" : canvas->get_type();
                mi::base::Handle<mi::neuraylib::ICanvas> gammaCanvas(
                    _pMdlSdk->imageApi()->convert(canvas.get(), targetPixelType.c_str()));
                gammaCanvas->set_gamma(texture->get_effective_gamma(0, 0));
                _pMdlSdk->imageApi()->adjust_gamma(gammaCanvas.get(), 1.0f);
                canvas = gammaCanvas;
            }
            else if (formatIt == pixelTypeToImageFormat.end())
            {
                canvas = _pMdlSdk->imageApi()->convert(canvas.get(), "Rgba");
            }

            // Create the image identifier
            auto filePath = image->get_filename(0, 0);
            if (filePath)
            {
                // The texture has a file path, create the identifier from it
                identifier = filesystem::path(filePath).filename().string();
                std::replace(identifier.begin(), identifier.end(), '.', '_');
            }
            else
            {
                // The texture is not an image file, create the identifier from the database name
                identifier = Foundation::replace(textureDbName, "::", "_");
            }

            // We already load image data when compiling the MDL material, so we
            // don't need the filename but rather fill in the init data directly.
            IImage::InitData initData;
            initData.linearize                   = false; // Always linearized already
            initData.isEnvironment               = false;
            // We are sure type exists now
            initData.format = pixelTypeToImageFormat.at(canvas->get_type());
            initData.name   = identifier;

            std::vector<uint8_t> pixelData;

            auto shape = targetCode->get_texture_shape(texIndex);
            if (shape == mi::neuraylib::ITarget_code::Texture_shape_2d)
            {
                // Fill init data with data from tile
                mi::base::Handle<const mi::neuraylib::ITile> tile(canvas->get_tile());

                initData.pImageData = tile->get_data();
                initData.width      = tile->get_resolution_x();
                initData.height     = tile->get_resolution_y();
            }
            else if (shape == mi::neuraylib::ITarget_code::Texture_shape_3d ||
                shape == mi::neuraylib::ITarget_code::Texture_shape_bsdf_data)
            {
                uint32_t width  = canvas->get_resolution_x();
                uint32_t height = canvas->get_resolution_y();
                uint32_t layers = canvas->get_layers_size();
                uint32_t bytesPerPixel =
                    _pMdlSdk->imageApi()->get_bytes_per_component(canvas->get_type()) *
                    _pMdlSdk->imageApi()->get_components_per_pixel(canvas->get_type());
                size_t layerSize = width * height * bytesPerPixel;

                pixelData.resize(layerSize * layers);
                for (mi::Uint32 layer = 0; layer < layers; layer++)
                {
                    mi::base::Handle<const mi::neuraylib::ITile> tile(canvas->get_tile(layer));
                    uint8_t* pixelDataPtr = pixelData.data() + layerSize * layer;
                    std::memcpy(pixelDataPtr, tile->get_data(), layerSize);
                }

                // Fill init data
                initData.pImageData = pixelData.data();
                initData.width      = width;
                initData.height     = height;
                initData.depth      = layers;
            }
            else
            {
                AU_WARN("Unsupported texture shape.");
                continue;
            }

            // Create texture and insert into cache
            auto auroraImage = _pRenderer->createImagePointer(initData);
            _imageCache.insert({ textureDbName, { identifier, auroraImage } });
            mdlData->textures.push_back(auroraImage);
        }

        TextureDefinition textureDef;
        textureDef.name = TextureIdentifier(identifier);
        // Keeping the filename empty since we already loaded the images
        textureDef.defaultFilename = "";

        defaultValues.textures.push_back(textureDef);
        defaultValues.textureNames.push_back(textureDef.name);
    }
}

} // nampespace MaterialXCodeGen

END_AURORA

#endif
