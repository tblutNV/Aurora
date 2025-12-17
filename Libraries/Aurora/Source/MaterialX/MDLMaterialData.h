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

BEGIN_AURORA

namespace MaterialXCodeGen
{

// Additional material data that is required for MDL materials.
struct MDLMaterialData
{
    std::vector<IImagePtr> textures;
    std::vector<uint8_t> roDataSegment;
    std::vector<uint8_t> defaultArgumentBlockData;
    std::vector<size_t> argumentBlockOffsets;
};

MAKE_AURORA_PTR(MDLMaterialData);

} // namespace MaterialXCodeGen

END_AURORA
