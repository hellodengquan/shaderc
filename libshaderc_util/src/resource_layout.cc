// Copyright 2026 The Shaderc Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "resource_layout.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace shaderc_util {
namespace resource_layout {
namespace {

// Describes a shared layout entry, e.g. "'albedo' (texture)".
std::string DescribeEntry(const Compiler::ResourceLayoutEntry& entry) {
  const char* kind = "unknown";
  switch (entry.kind) {
    case Compiler::UniformKind::Image:
      kind = "image";
      break;
    case Compiler::UniformKind::Sampler:
      kind = "sampler";
      break;
    case Compiler::UniformKind::Texture:
      kind = "texture";
      break;
    case Compiler::UniformKind::Buffer:
      kind = "uniform buffer";
      break;
    case Compiler::UniformKind::StorageBuffer:
      kind = "storage buffer";
      break;
    case Compiler::UniformKind::UnorderedAccessView:
      kind = "unordered access view";
      break;
  }
  std::ostringstream out;
  out << "'" << entry.name << "' (" << kind << ")";
  return out.str();
}

// Describes a descriptor slot, e.g. "set 1, binding 2".
std::string DescribeSlot(int set, int binding) {
  std::ostringstream out;
  out << "set " << set << ", binding " << binding;
  return out.str();
}

}  // anonymous namespace

ResourceType ClassifyResource(const glslang::TType& type) {
  const glslang::TBasicType basic = type.getBasicType();
  if (basic == glslang::EbtSampler) {
    const glslang::TSampler& sampler = type.getSampler();
    if (sampler.isImage()) return ResourceType::kImage;
    if (sampler.isPureSampler()) return ResourceType::kSampler;
    if (sampler.isCombined()) return ResourceType::kCombinedSampler;
    if (sampler.isTexture() || sampler.isSubpass()) {
      return ResourceType::kTexture;
    }
    return ResourceType::kOther;
  }
  if (basic == glslang::EbtAccStruct) return ResourceType::kAccelStruct;
  if (type.isTensorARM()) return ResourceType::kTensor;
  // A plain uniform struct (not a block) takes a location rather than a
  // binding, so it is not a descriptor resource here.
  if (type.getQualifier().storage == glslang::EvqUniform &&
      basic == glslang::EbtBlock) {
    return ResourceType::kUbo;
  }
  if (type.getQualifier().storage == glslang::EvqBuffer) {
    return ResourceType::kSsbo;
  }
  return ResourceType::kOther;
}

bool IsDescriptorResource(ResourceType type) {
  switch (type) {
    case ResourceType::kSampler:
    case ResourceType::kTexture:
    case ResourceType::kCombinedSampler:
    case ResourceType::kImage:
    case ResourceType::kUbo:
    case ResourceType::kSsbo:
    case ResourceType::kAccelStruct:
    case ResourceType::kTensor:
      return true;
    case ResourceType::kOther:
      return false;
  }
  return false;
}

bool ToUniformKind(ResourceType type, Compiler::UniformKind* kind) {
  switch (type) {
    case ResourceType::kSampler:
      *kind = Compiler::UniformKind::Sampler;
      return true;
    case ResourceType::kTexture:
    case ResourceType::kCombinedSampler:
      // A combined image sampler is a sampled texture as far as a material's
      // resource layout is concerned.
      *kind = Compiler::UniformKind::Texture;
      return true;
    case ResourceType::kImage:
      *kind = Compiler::UniformKind::Image;
      return true;
    case ResourceType::kUbo:
      *kind = Compiler::UniformKind::Buffer;
      return true;
    case ResourceType::kSsbo:
      *kind = Compiler::UniformKind::StorageBuffer;
      return true;
    case ResourceType::kAccelStruct:
    case ResourceType::kTensor:
      // No shared-layout kind describes an acceleration structure or a tensor,
      // so such a resource is never matched to an entry.
      return false;
    case ResourceType::kOther:
      return false;
  }
  return false;
}

std::vector<ResourceType> Resolver::TypesForKind(Compiler::UniformKind kind) {
  switch (kind) {
    case Compiler::UniformKind::Sampler:
      return {ResourceType::kSampler};
    case Compiler::UniformKind::Texture:
      // A layout entry for a texture may name either a separate texture or a
      // combined image sampler, so reserve its slot in both binding classes.
      return {ResourceType::kTexture, ResourceType::kCombinedSampler};
    case Compiler::UniformKind::Image:
      return {ResourceType::kImage};
    case Compiler::UniformKind::Buffer:
      return {ResourceType::kUbo};
    case Compiler::UniformKind::StorageBuffer:
      return {ResourceType::kSsbo};
    case Compiler::UniformKind::UnorderedAccessView:
      // Shaderc classifies every GLSL descriptor resource as a sampler,
      // texture, image, UBO, or SSBO; there is no separate GLSL UAV class.  An
      // entry of this kind therefore matches nothing in GLSL and reserves no
      // slot, which keeps it consistent with ToUniformKind.
      return {};
  }
  return {};
}

Resolver::Resolver(std::unique_ptr<glslang::TIoMapResolver> delegate,
                   const std::vector<Compiler::ResourceLayoutEntry>& entries,
                   const uint32_t* auto_binding_base,
                   bool per_resource_type_namespaces)
    : delegate_(std::move(delegate)),
      entries_(entries),
      auto_binding_base_(auto_binding_base),
      per_resource_type_namespaces_(per_resource_type_namespaces) {}

Resolver::~Resolver() = default;

Resolver::Resource* Resolver::FindResource(EShLanguage stage,
                                           const std::string& name) {
  auto stage_it = resources_.find(stage);
  if (stage_it == resources_.end()) return nullptr;
  auto at = stage_it->second.find(name);
  return at == stage_it->second.end() ? nullptr : &at->second;
}

int Resolver::NamespaceKey(ResourceType type) const {
  // When each resource type has its own binding namespace (OpenGL), resources
  // of different types may reuse a binding number.  Otherwise all resources of
  // a set share one namespace (Vulkan).
  return per_resource_type_namespaces_ ? static_cast<int>(type) : 0;
}

int Resolver::BindingCount(const glslang::TType& type) const {
  // In OpenGL, arrays of opaque types take one binding per element.  In Vulkan
  // an array occupies a single binding.  This mirrors glslang.
  if (per_resource_type_namespaces_ && type.isSizedArray()) {
    return type.getCumulativeArraySize();
  }
  return 1;
}

uint32_t Resolver::BaseBinding(ResourceType type) const {
  Compiler::UniformKind kind;
  if (type == ResourceType::kCombinedSampler) {
    // glslang has no shaderc binding base for combined image samplers.
    return 0;
  }
  if (type == ResourceType::kAccelStruct || type == ResourceType::kTensor) {
    // Acceleration structures and tensors have no shaderc binding base.
    return 0;
  }
  if (!ToUniformKind(type, &kind)) return 0;
  return auto_binding_base_[static_cast<int>(kind)];
}

bool Resolver::IsCoveredByAgreeingExplicit(
    EShLanguage stage, const Compiler::ResourceLayoutEntry& entry,
    int namespace_key) const {
  auto stage_it = resources_.find(stage);
  if (stage_it == resources_.end()) return false;
  for (const auto& name_and_resource : stage_it->second) {
    const Resource& resource = name_and_resource.second;
    // Only a resource matched to this exact entry can cover it.  A resource
    // whose explicit binding differs from the entry is already a reported
    // conflict and must not suppress reserving the entry's slot.
    if (resource.matched_entry != &entry) continue;
    if (!resource.has_explicit_binding) continue;
    if (NamespaceKey(resource.type) != namespace_key) continue;
    if (resource.explicit_set == static_cast<int>(entry.set) &&
        resource.explicit_binding == static_cast<int>(entry.binding)) {
      return true;
    }
  }
  return false;
}

void Resolver::AddConflict(const glslang::TSourceLoc& loc,
                           const std::string& message) {
  for (const auto& existing : conflicts_) {
    if (existing.message == message) return;
  }
  Conflict conflict;
  conflict.loc = loc;
  conflict.message = message;
  conflicts_.push_back(std::move(conflict));
}

void Resolver::RecordResource(EShLanguage stage, glslang::TVarEntryInfo& ent) {
  const glslang::TType& type = ent.symbol->getType();
  if (!type.getQualifier().isUniformOrBuffer() ||
      type.getQualifier().isPushConstant() ||
      type.getQualifier().isShaderRecord()) {
    return;
  }
  const std::string name(ent.symbol->getAccessName().c_str());
  auto& stage_resources = resources_[stage];
  auto at = stage_resources.find(name);
  if (at == stage_resources.end()) {
    Resource resource;
    resource.loc = ent.symbol->getLoc();
    at = stage_resources.emplace(name, std::move(resource)).first;
    order_[stage].push_back(name);
  }
  Resource& resource = at->second;
  resource.type = ClassifyResource(type);
  // A resource counts as live if any symbol node referring to it is live.
  resource.live = resource.live || ent.live;
  // Interface blocks are known by their block name in glslang's access name,
  // but a material layout naturally names them by the instance name (or the
  // block name when there is no instance name).  Remember the instance name so
  // an entry can match either.
  const std::string instance_name(ent.symbol->getName().c_str());
  if (instance_name != name) resource.alt_name = instance_name;

  // Let the delegate choose the default set; use it when the shader does not
  // assign one.
  ent.newSet = -1;
  resource.default_set = delegate_->resolveSet(stage, ent);
  resource.has_explicit_set = type.getQualifier().hasSet();
  resource.explicit_set =
      resource.has_explicit_set ? type.getQualifier().layoutSet
                                : resource.default_set;
  resource.has_explicit_binding = type.getQualifier().hasBinding();
  if (resource.has_explicit_binding) {
    resource.explicit_binding = static_cast<int>(BaseBinding(resource.type)) +
                                type.getQualifier().layoutBinding;
  }
  resource.binding_count = BindingCount(type);
}

void Resolver::Occupy(EShLanguage stage, int namespace_key, int set, int slot,
                      int size, const Occupant& occupant,
                      const glslang::TSourceLoc& loc) {
  SlotMap& slots = occupied_[stage][std::make_pair(namespace_key, set)];
  for (int i = 0; i < size; ++i) {
    auto at = slots.find(slot + i);
    if (at == slots.end()) {
      slots[slot + i] = occupant;
      continue;
    }
    const Occupant& other = at->second;
    // Re-reserving the same slot for the same resource (for example, an
    // explicit binding that agrees with the layout) is not a conflict.
    if (other.description == occupant.description) continue;
    // Aliasing between two explicit bindings is tolerated, matching glslang.
    // A collision is reported only when a layout reservation and an explicit
    // binding claim the same slot for different resources.  Automatic
    // reservations never reach here, because allocation skips reserved slots.
    const bool layout_involved =
        occupant.owner == Owner::kLayout || other.owner == Owner::kLayout;
    const bool explicit_involved =
        occupant.owner == Owner::kExplicit || other.owner == Owner::kExplicit;
    if (layout_involved && explicit_involved) {
      std::ostringstream message;
      message << occupant.description << " uses " << DescribeSlot(set, slot + i)
              << ", which conflicts with " << other.description;
      AddConflict(loc, message.str());
    }
  }
}

int Resolver::Allocate(EShLanguage stage, int namespace_key, int set,
                       uint32_t base, int size) {
  SlotMap& slots = occupied_[stage][std::make_pair(namespace_key, set)];
  int candidate = static_cast<int>(base);
  // Find the first run of |size| consecutive free bindings at or above |base|.
  for (;;) {
    int next_candidate = candidate;
    for (int i = 0; i < size; ++i) {
      auto at = slots.find(candidate + i);
      if (at != slots.end()) {
        next_candidate = std::max(next_candidate, at->first + 1);
      }
    }
    if (next_candidate == candidate) break;
    candidate = next_candidate;
  }
  Occupant automatic;
  automatic.owner = Owner::kAutomatic;
  for (int i = 0; i < size; ++i) {
    slots[candidate + i] = automatic;
  }
  return candidate;
}

void Resolver::PlanStage(EShLanguage stage) {
  if (planned_[stage]) return;
  planned_[stage] = true;
  auto res_it = resources_.find(stage);
  if (res_it == resources_.end()) return;
  auto& resources = res_it->second;

  // Index the shared layout entries by name.  A name can appear once per kind;
  // a duplicated (name, kind) that assigns a different slot is ambiguous.
  std::map<std::string, std::vector<const Compiler::ResourceLayoutEntry*>>
      entries_by_name;
  for (const auto& entry : entries_) {
    entries_by_name[entry.name].push_back(&entry);
  }

  // Finds the layout entry matching |name| and |kind|.  Sets *ambiguous when
  // several matching entries assign different slots.  Returns null when no
  // entry matches.
  using Entry = Compiler::ResourceLayoutEntry;
  auto find_entry = [&entries_by_name](const std::string& name,
                                       Compiler::UniformKind kind,
                                       bool* ambiguous) -> const Entry* {
    auto by_name = entries_by_name.find(name);
    if (by_name == entries_by_name.end()) return nullptr;
    const Compiler::ResourceLayoutEntry* match = nullptr;
    for (const auto* entry : by_name->second) {
      if (entry->kind != kind) continue;
      if (match == nullptr) {
        match = entry;
      } else if (match->set != entry->set ||
                 match->binding != entry->binding) {
        *ambiguous = true;
      }
    }
    return match;
  };

  // Match each resource to an entry of the right kind.  An interface block may
  // be named by its block name or its instance name.
  for (const auto& name : order_[stage]) {
    Resource& resource = resources[name];
    Compiler::UniformKind kind;
    if (!ToUniformKind(resource.type, &kind)) continue;
    bool ambiguous = false;
    const Compiler::ResourceLayoutEntry* match =
        find_entry(name, kind, &ambiguous);
    if (match == nullptr && !resource.alt_name.empty()) {
      match = find_entry(resource.alt_name, kind, &ambiguous);
    }
    if (match == nullptr) continue;

    if (ambiguous) {
      std::ostringstream message;
      message << "the shared resource layout contains more than one entry for "
              << DescribeEntry(*match) << ", so the entry is ambiguous";
      AddConflict(resource.loc, message.str());
      continue;
    }

    // The shader's explicit layout takes precedence.  When it disagrees with
    // the shared layout, report the conflict naming both sides and leave the
    // resource to the default rules.
    if (resource.has_explicit_binding &&
        resource.explicit_binding != static_cast<int>(match->binding)) {
      std::ostringstream message;
      message << "resource '" << name << "' explicitly uses "
              << DescribeSlot(resource.explicit_set, resource.explicit_binding)
              << ", which conflicts with the shared resource layout entry for "
              << DescribeEntry(*match) << " at "
              << DescribeSlot(match->set, match->binding);
      AddConflict(resource.loc, message.str());
      continue;
    }
    if (resource.has_explicit_set &&
        resource.explicit_set != static_cast<int>(match->set)) {
      std::ostringstream message;
      message << "resource '" << name << "' explicitly uses "
              << DescribeSlot(resource.explicit_set,
                              resource.has_explicit_binding
                                  ? resource.explicit_binding
                                  : static_cast<int>(match->binding))
              << ", which conflicts with the shared resource layout entry for "
              << DescribeEntry(*match) << " at "
              << DescribeSlot(match->set, match->binding);
      AddConflict(resource.loc, message.str());
      continue;
    }
    resource.has_entry = true;
    resource.entry_set = static_cast<int>(match->set);
    resource.entry_binding = static_cast<int>(match->binding);
    resource.matched_entry = match;
  }

  // Reserve every explicit binding first, so the shader's explicit layout
  // always wins.
  for (const auto& name : order_[stage]) {
    Resource& resource = resources[name];
    if (!resource.has_explicit_binding) continue;
    resource.final_set = resource.explicit_set;
    resource.final_binding = resource.explicit_binding;
    Occupant occupant;
    occupant.owner = Owner::kExplicit;
    occupant.description = "resource '" + name + "'";
    Occupy(stage, NamespaceKey(resource.type), resource.explicit_set,
           resource.explicit_binding, resource.binding_count, occupant,
           resource.loc);
  }

  // Reserve every shared layout slot that could apply to this stage, whether or
  // not the stage actually declares the resource.  Reserving the whole layout,
  // not just the matched resources, is what keeps the same name on the same
  // slot across stages and keeps a resource only this stage can see from
  // occupying a slot the layout reserves for another stage.  A slot that an
  // agreeing explicit binding already claimed is skipped, so the two are not
  // reported as a conflict.
  for (const auto& entry : entries_) {
    Occupant occupant;
    occupant.owner = Owner::kLayout;
    occupant.description =
        "the shared resource layout entry for " + DescribeEntry(entry);
    for (ResourceType type : TypesForKind(entry.kind)) {
      if (IsCoveredByAgreeingExplicit(stage, entry, NamespaceKey(type))) {
        continue;
      }
      Occupy(stage, NamespaceKey(type), static_cast<int>(entry.set),
             static_cast<int>(entry.binding), 1, occupant,
             glslang::TSourceLoc());
    }
  }

  // Assign the resources that matched a layout entry.  A conflict with the
  // layout was already reported, in which case the resource is left to the
  // default rules below.
  for (const auto& name : order_[stage]) {
    Resource& resource = resources[name];
    if (!resource.has_entry || resource.has_explicit_binding) continue;
    resource.final_set = resource.entry_set;
    resource.final_binding = resource.entry_binding;
  }

  // Assign everything else by automatic binding, in declaration order, from
  // the per-kind base, skipping every reserved slot.  This includes every
  // live descriptor resource, not just those a layout entry names, so a
  // resource that no entry describes (for example an acceleration structure)
  // still cannot land on a slot the layout reserves.
  for (const auto& name : order_[stage]) {
    Resource& resource = resources[name];
    if (resource.final_binding >= 0 || resource.has_entry) continue;
    if (!IsDescriptorResource(resource.type)) continue;
    if (!resource.live) continue;
    const int set = resource.has_explicit_set ? resource.explicit_set
                                              : resource.default_set;
    resource.final_set = set;
    resource.final_binding =
        Allocate(stage, NamespaceKey(resource.type), set,
                 BaseBinding(resource.type), resource.binding_count);
  }
}

bool Resolver::validateBinding(EShLanguage stage, glslang::TVarEntryInfo& ent) {
  return delegate_->validateBinding(stage, ent);
}

int Resolver::resolveBinding(EShLanguage stage, glslang::TVarEntryInfo& ent) {
  PlanStage(stage);
  Resource* resource =
      FindResource(stage, std::string(ent.symbol->getAccessName().c_str()));
  if (resource && resource->final_binding >= 0) {
    return ent.newBinding = resource->final_binding;
  }
  return delegate_->resolveBinding(stage, ent);
}

int Resolver::resolveSet(EShLanguage stage, glslang::TVarEntryInfo& ent) {
  PlanStage(stage);
  Resource* resource =
      FindResource(stage, std::string(ent.symbol->getAccessName().c_str()));
  if (resource && resource->final_binding >= 0) {
    return ent.newSet = resource->final_set;
  }
  return delegate_->resolveSet(stage, ent);
}

int Resolver::resolveUniformLocation(EShLanguage stage,
                                     glslang::TVarEntryInfo& ent) {
  return delegate_->resolveUniformLocation(stage, ent);
}

bool Resolver::validateInOut(EShLanguage stage, glslang::TVarEntryInfo& ent) {
  return delegate_->validateInOut(stage, ent);
}

int Resolver::resolveInOutLocation(EShLanguage stage,
                                   glslang::TVarEntryInfo& ent) {
  return delegate_->resolveInOutLocation(stage, ent);
}

int Resolver::resolveInOutComponent(EShLanguage stage,
                                    glslang::TVarEntryInfo& ent) {
  return delegate_->resolveInOutComponent(stage, ent);
}

int Resolver::resolveInOutIndex(EShLanguage stage,
                                glslang::TVarEntryInfo& ent) {
  return delegate_->resolveInOutIndex(stage, ent);
}

void Resolver::notifyBinding(EShLanguage stage, glslang::TVarEntryInfo& ent) {
  RecordResource(stage, ent);
  delegate_->notifyBinding(stage, ent);
}

void Resolver::notifyInOut(EShLanguage stage, glslang::TVarEntryInfo& ent) {
  delegate_->notifyInOut(stage, ent);
}

void Resolver::beginNotifications(EShLanguage stage) {
  delegate_->beginNotifications(stage);
}

void Resolver::endNotifications(EShLanguage stage) {
  delegate_->endNotifications(stage);
}

void Resolver::beginResolve(EShLanguage stage) {
  delegate_->beginResolve(stage);
}

void Resolver::endResolve(EShLanguage stage) { delegate_->endResolve(stage); }

void Resolver::beginCollect(EShLanguage stage) {
  delegate_->beginCollect(stage);
}

void Resolver::endCollect(EShLanguage stage) { delegate_->endCollect(stage); }

void Resolver::reserverStorageSlot(glslang::TVarEntryInfo& ent,
                                   TInfoSink& infoSink) {
  delegate_->reserverStorageSlot(ent, infoSink);
}

void Resolver::reserverResourceSlot(glslang::TVarEntryInfo& ent,
                                    TInfoSink& infoSink) {
  delegate_->reserverResourceSlot(ent, infoSink);
}

void Resolver::addStage(EShLanguage stage,
                        glslang::TIntermediate& intermediate) {
  delegate_->addStage(stage, intermediate);
}

}  // namespace resource_layout
}  // namespace shaderc_util
