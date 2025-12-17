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
#include "pch.h"

#include "UniformBuffer.h"

BEGIN_AURORA

const UniformBufferPropertyDefinition* UniformBuffer::getPropertyDef(
    const string& propertyName) const
{
    // Look up the the first in the field map. return null if not found.
    auto iter = _fieldMap.find(propertyName);
    if (iter == _fieldMap.end())
        return nullptr;

    // Get the property for the field.
    size_t fieldIndex = iter->second;
    int propertyIndex = _fields[fieldIndex].index;
    return &_definition[propertyIndex];
}

const UniformBuffer::Field* UniformBuffer::getField(const string& propertyName) const
{
    // Get pointer to field or null if not found.
    auto iter = _fieldMap.find(propertyName);
    if (iter == _fieldMap.end())
        return nullptr;
    size_t fieldIndex = iter->second;
    return &_fields[fieldIndex];
}

size_t UniformBuffer::getOffset(const string& propertyName) const
{
    // Get field
    auto pField = getField(propertyName);

    // Return offset (multiplied by word size) or -1 if not found.
    return pField ? pField->bufferOffset * sizeof(_data[0]) : -1;
}

size_t UniformBuffer::getIndex(const string& propertyName) const
{
    // Get field
    auto pField = getField(propertyName);

    // Return offset (multiplied by word size) or -1 if not found.
    return pField ? pField->index : -1;
}

size_t UniformBuffer::getOffsetForVariable(const string& varName) const
{
    // Get offset, looked up by variable name.
    auto iter = _fieldVariableMap.find(varName);
    if (iter == _fieldVariableMap.end())
        return (size_t)-1;
    size_t fieldIndex = iter->second;
    return _fields[fieldIndex].bufferOffset * sizeof(_data[0]);
}

string UniformBuffer::generateHLSLStruct() const
{
    // Create struct using stringstream
    stringstream ss;

    // String begins at first curly brace (no struct or name)
    ss << "{" << endl;

    // Iterate through fields, keeping track of padding.
    int paddingIndex = 0;
    for (size_t i = 0; i < _fields.size(); i++)
    {

        // If field is padding add padding int (index is -1).
        if (_fields[i].index == -1)
        {
            ss << "\tint _padding" << paddingIndex++ << ";" << endl;
        }
        else
        {
            // If get definition for field.
            auto def = _definition[_fields[i].index];

            // Sdd field type and variable name.
            ss << "\t" << getHLSLStringFromType(def.type) << " " << def.variableName << ";";

            // Add comment.
            ss << " // Offset:" << (_fields[i].bufferOffset * sizeof(_data[0]))
               << " Property:" << def.name << endl;
        }
    }

    // Closing brace.
    ss << "}" << endl;

    // Return string.
    return ss.str();
}

string UniformBuffer::generateHLSLStructAndAccessors(
    const string& structName, const string& prefix) const
{
    // Create struct using stringstream
    stringstream ss;

    // String begins at first curly brace (no struct or name)
    ss << "struct " << structName << endl;
    ss << generateHLSLStruct();
    ss << ";" << endl;

    for (size_t i = 0; i < _fields.size(); i++)
    {
        if (_fields[i].index != -1)
        {
            // If get definition for field.
            auto def = _definition[_fields[i].index];
            ss << endl
               << getHLSLStringFromType(def.type) << " " << prefix << def.variableName << "("
               << structName << " mtl, int /*materialOffset*/) {" << endl
               << "\treturn mtl." << def.variableName << ";" << endl;
            ss << "}" << endl;
        }
    }

    // Return string.
    return ss.str();
}

string UniformBuffer::generateByteAddressBufferAccessors(const string& prefix) const
{
    stringstream ss;
    for (size_t i = 0; i < _fields.size(); i++)
    {

        if (_fields[i].index == -1)
            continue;
        auto def = _definition[_fields[i].index];

        ss << " // Get property " << def.name << " from byte address buffer" << endl;
        ss << getHLSLStringFromType(def.type) << " " << prefix << def.variableName
           << "(ByteAddressBuffer buf, int materialOffset = 0) {" << endl;
        size_t offset = _fields[i].bufferOffset * sizeof(_data[0]);
        switch (def.type)
        {
        case PropertyValue::Type::Bool:
        case PropertyValue::Type::Int:
            ss << "\treturn buf.Load(materialOffset + " << offset << ");" << endl;
            break;
        case PropertyValue::Type::Float:
            ss << "\treturn asfloat(buf.Load(materialOffset + " << offset << "));" << endl;
            break;
        case PropertyValue::Type::Float2:
            ss << "\treturn asfloat(buf.Load2(materialOffset + " << offset << "));" << endl;
            break;
        case PropertyValue::Type::Float3:
            ss << "\treturn asfloat(buf.Load3(materialOffset + " << offset << "));" << endl;
            break;
        case PropertyValue::Type::Float4:
            ss << "\treturn asfloat(buf.Load4(materialOffset + " << offset << "));" << endl;
            break;
        case PropertyValue::Type::Matrix4:
            for (int j = 0; j < 16; j++)
            {
                ss << "\tfloat4 m" << j << " = asfloat(buf.Load(materialOffset + "
                   << offset + (j * 4) << "));" << endl;
                ;
            }
            ss << "float4x4 mtx = {";

            for (int j = 0; j < 16; j++)
            {
                ss << "m" << j;
                if (j < 15)
                    ss << ", ";
            }
            ss << "};" << endl;
            ss << "\treturn mtx;" << endl;
            break;
        default:
            break;
        }

        ss << "}" << endl << endl;
    }

    return ss.str();
}

void UniformBuffer::reset(const string& name)
{
    auto pField  = getField(name);
    if (!pField)
    {
        AU_ERROR("Unknown property %s", name.c_str());
        return;
    }

#if ENABLE_MATERIALX && ENABLE_MDL
    if (_defaultMdlArgBlock)
    {
        set(name, (*_defaultMdlArgBlock)[pField->bufferOffset]);
    }
    else
#endif
    {
        set(name, (*_defaults)[pField->index]);
    }
}

PropertyValue::Type UniformBuffer::getType(const string& propertyName) const
{
    auto pDef = getPropertyDef(propertyName);
    if (!pDef)
        return PropertyValue::Type::Undefined;
    return pDef->type;
}

const string& UniformBuffer::getVariableName(const string& propertyName) const
{
    static string invalidName = "";
    auto pDef                 = getPropertyDef(propertyName);
    if (!pDef)
        return invalidName;
    return pDef->variableName;
}

UniformBuffer::UniformBuffer(
    const UniformBufferDefinition& definition, const vector<PropertyValue>& defaults) :
    _definition(definition), _defaults(&defaults)
{
    AU_ASSERT(_definition.size() == _defaults->size(),
        "Mismatch between defaults size and definitions size");

    size_t wordSize     = sizeof(_data[0]);
    size_t bufferOffset = 0;
    for (size_t i = 0; i < definition.size(); i++)
    {
        // Get size and alignment of current property, in bytes.
        PropertyValue::Type type = definition[i].type;
        size_t valAlignment      = getAlignment(type);
        size_t valSize           = getSizeOfType(type);
        AU_ASSERT(valSize % wordSize == 0, "Type too small for uniform buffer");

        // HLSL packing ensures properties (that are quadword size or less) cannot cross quad-word
        // boundary.
        size_t endOffset             = bufferOffset + valSize - wordSize;
        bool crossesQuadWordBoundary = (valSize <= 16) && (endOffset / 16 != bufferOffset / 16);

        // Add padding while alignment incorrect and field is crossing quad-word boundary.
        while (bufferOffset % valAlignment || crossesQuadWordBoundary)
        {
            // Add padding field.
            _fields.push_back({ bufferOffset, PropertyValue::Type::Int, -1 });
            bufferOffset += wordSize;

            // See if we still cross quad word boundary.
            endOffset               = bufferOffset + valSize - wordSize;
            crossesQuadWordBoundary = (valSize <= 16) && (endOffset / 16 != bufferOffset / 16);
        }

        // Create field.
        int fieldIndex                                = (int)_fields.size();
        _fieldMap[definition[i].name]                 = fieldIndex;
        _fieldVariableMap[definition[i].variableName] = fieldIndex;
        _fields.push_back({ bufferOffset, definition[i].type, (int)i });

        // Set default values.
        AU_ASSERT(
            _definition[i].type == (*_defaults)[i].type, "Default type does not match definition");
        bufferOffset = copyToBuffer(defaults[i], bufferOffset);
    }

    // Align buffer to 16 bytes.
    while (bufferOffset % 16)
    {
        _fields.push_back({ bufferOffset, PropertyValue::Type::Int, -1 });
        bufferOffset += wordSize;
    }

    // Ensure the final padding fields are included in buffer size.
    _data.resize(bufferOffset / wordSize);
}

#if ENABLE_MATERIALX && ENABLE_MDL
UniformBuffer::UniformBuffer(const UniformBufferDefinition& definition,
    const std::vector<size_t>& offsets, const vector<uint8_t>& defaults) :
    _definition(definition), _defaultMdlArgBlock(&defaults)
{
    for (size_t i = 0; i < definition.size(); i++)
    {
        // Create field.
        int fieldIndex                                = (int)_fields.size();
        _fieldMap[definition[i].name]                 = fieldIndex;
        _fieldVariableMap[definition[i].variableName] = fieldIndex;
        _fields.push_back({ offsets[i], definition[i].type, (int)i });
    }

    // Set default values.
    _data = defaults;
}
#endif

const UniformBuffer::Field* UniformBuffer::findField(const string& name)
{
    // Find the index of field, return null if nor found.
    auto iter = _fieldMap.find(name);
    if (iter == _fieldMap.end())
        return nullptr;

    // Return the indexed field.
    return &_fields[iter->second];
}

void UniformBuffer::set(const string& name, const PropertyValue& val)
{
    // Find the field for given name,  print error and return if not found.
    const UniformBuffer::Field* pField = findField(name);
    if (!pField)
    {
        AU_ERROR("Uniform block does not contain property %s", name.c_str());
        return;
    }

    // If the value's type does not match field's print error and return.
    if (pField->type != val.type)
    {
        AU_ERROR("Type mismatch in UniformBlock for property %s, %x!=%x", name.c_str(),
            pField->type, val.type);
        return;
    }

    // Copy the value to the buffer at the offset given by bufferOffset.
    copyToBuffer(val, pField->bufferOffset);
}

size_t UniformBuffer::copyToBuffer(const PropertyValue& val, size_t bufferOffset)
{
    switch (val.type)
    {
    case PropertyValue::Type::Bool:
        return copyToBuffer<int>(val.asBool(), bufferOffset);
    case PropertyValue::Type::Int:
        return copyToBuffer(val.asInt(), bufferOffset);
    case PropertyValue::Type::Float:
        return copyToBuffer(val.asFloat(), bufferOffset);
    case PropertyValue::Type::Float2:
        return copyToBuffer(val.asFloat2(), bufferOffset);
    case PropertyValue::Type::Float3:
        return copyToBuffer(val.asFloat3(), bufferOffset);
    case PropertyValue::Type::Float4:
        return copyToBuffer(val.asFloat4(), bufferOffset);
    case PropertyValue::Type::Matrix4:
        return copyToBuffer(val.asMatrix4(), bufferOffset);
    default:
        AU_FAIL("Unsupported type for uniform block:%x", val.type);
        return 0;
    }
}

// Following HLSL alignment rules.
size_t UniformBuffer::getAlignment(PropertyValue::Type type)
{
    // Scalars and vectors of 3 or less fields have single word alignment, everything else is
    // quadword.
    switch (type)
    {
    case PropertyValue::Type::Bool:
        return sizeof(int);
    case PropertyValue::Type::Int:
        return sizeof(int);
    case PropertyValue::Type::Float:
        return sizeof(float);
    case PropertyValue::Type::Float2:
        return sizeof(float);
    case PropertyValue::Type::Float3:
        return sizeof(float);
    case PropertyValue::Type::Float4:
        return sizeof(vec4);
    case PropertyValue::Type::Matrix4:
        return sizeof(vec4);
    default:
        AU_FAIL("Unsupported type for uniform block:%x", type);
        return 0;
    }
}

string UniformBuffer::getGLSLStringFromType(PropertyValue::Type type)
{
    switch (type)
    {
    case PropertyValue::Type::Bool:
        return "int";
    case PropertyValue::Type::Int:
        return "int";
    case PropertyValue::Type::Float:
        return "float";
    case PropertyValue::Type::Float2:
        return "vec2";
    case PropertyValue::Type::Float3:
        return "vec3";
    case PropertyValue::Type::Float4:
        return "vec4";
    case PropertyValue::Type::Matrix4:
        return "mat4";
    default:
        AU_FAIL("Unsupported type for uniform block:%x", type);
        return {};
    }
}

string UniformBuffer::getHLSLStringFromType(PropertyValue::Type type)
{
    switch (type)
    {
    case PropertyValue::Type::Bool:
        return "int";
    case PropertyValue::Type::Int:
        return "int";
    case PropertyValue::Type::Float:
        return "float";
    case PropertyValue::Type::Float2:
        return "float2";
    case PropertyValue::Type::Float3:
        return "float3";
    case PropertyValue::Type::Float4:
        return "float4";
    case PropertyValue::Type::Matrix4:
        return "float4x4";
    default:
        AU_FAIL("Unsupported type for uniform block:%x", type);
        return {};
    }
}

size_t UniformBuffer::getSizeOfType(PropertyValue::Type type)
{
    switch (type)
    {
    case PropertyValue::Type::Bool:
        return sizeof(int);
    case PropertyValue::Type::Int:
        return sizeof(int);
    case PropertyValue::Type::Float:
        return sizeof(float);
    case PropertyValue::Type::Float2:
        return sizeof(vec2);
    case PropertyValue::Type::Float3:
        return sizeof(vec3);
    case PropertyValue::Type::Float4:
        return sizeof(vec4);
    case PropertyValue::Type::Matrix4:
        return sizeof(mat4);
    default:
        AU_FAIL("Unsupported type for uniform block:%x", type);
        return 0;
    }
}

END_AURORA
