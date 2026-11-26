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

#ifndef LIBSHADERC_UTIL_SRC_RESOURCE_LAYOUT_H_
#define LIBSHADERC_UTIL_SRC_RESOURCE_LAYOUT_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "glslang/Include/intermediate.h"
#include "glslang/Public/ShaderLang.h"
#include "libshaderc_util/compiler.h"

namespace shaderc_util {
namespace resource_layout {

// The kind of a binding-based uniform resource, following the classification
// glslang uses for automatic binding assignment.  A combined image sampler and
// a separate texture are distinguished even though a shared layout describes
// both as Compiler::UniformKind::Texture, because they consume different
// automatic binding bases.
enum class ResourceType {
  kSampler,          // A pure sampler.
  kTexture,          // A separate texture, or a subpass input.
  kCombinedSampler,  // A combined image sampler.
  kImage,            // An image or image buffer.
  kUbo,              // A descriptor-backed uniform block.
  kSsbo,             // A shader storage block.
  kAccelStruct,      // A ray tracing acceleration structure.
  kTensor,           // A tensor.
  kOther,            // Not a descriptor-backed resource.
};

// Returns true when the resource consumes a descriptor binding, so it competes
// for a slot in a descriptor set.  Resources that do not (plain uniforms,
// atomic counters) are left entirely to glslang.
bool IsDescriptorResource(ResourceType type);

// Returns the resource type of a uniform or buffer variable, following the
// classification glslang uses when automatically assigning bindings.
ResourceType ClassifyResource(const glslang::TType& type);

// Maps a resource type to the shared-layout uniform kind.  Returns false when
// the resource cannot be described by a shared layout.
bool ToUniformKind(ResourceType type, Compiler::UniformKind* kind);

// A conflict between the explicit layout in a shader and a shared resource
// layout.  The message names the resource and the shared layout entry that
// disagree.
struct Conflict {
  // Source location of the declared resource, for use as a diagnostic prefix.
  // Empty (line 0, no name) when no single declaration is at fault.
  glslang::TSourceLoc loc;
  // A complete, user-facing message, without a leading "error:".
  std::string message;
};

// Places the descriptor set and binding of each uniform resource for one
// compiled stage, so that resources described by a shared resource layout use
// the layout's set and binding.  Everything other than the descriptor set and
// binding of resources is delegated to |delegate|, which owns glslang's
// default rules for explicit bindings, uniform locations, and input/output
// locations.
//
// The shared layout acts as a set of reserved slots that every stage honours,
// even for resources it does not itself declare.  A resource without an
// explicit binding is first matched by name and kind against the layout; the
// remaining resources are then assigned the lowest free binding at or above
// their per-kind base, in declaration order.  Because the layout's slots are
// reserved in every stage, a resource only one stage can see never shares a
// slot with a material resource, and the slots of material resources are the
// same in every stage.
class Resolver final : public glslang::TIoMapResolver {
 public:
  // Takes ownership of |delegate|.  |entries| are the shared layout entries
  // for the stage being compiled and may be empty.  |auto_binding_base| is
  // indexed by Compiler::UniformKind and holds the lowest automatically
  // assigned binding number for the stage being compiled; it must outlive the
  // resolver.  |per_resource_type_namespaces| is true when each resource type
  // has its own binding namespace within a set (OpenGL) and false when all
  // resource types share one namespace per set (Vulkan).
  Resolver(std::unique_ptr<glslang::TIoMapResolver> delegate,
           const std::vector<Compiler::ResourceLayoutEntry>& entries,
           const uint32_t* auto_binding_base,
           bool per_resource_type_namespaces);
  ~Resolver() override;

  // Conflicts recorded while planning.  Empty when the explicit layout and the
  // shared layout are consistent.
  const std::vector<Conflict>& conflicts() const { return conflicts_; }

  // glslang::TIoMapResolver interface, in declaration order.
  bool validateBinding(EShLanguage stage, glslang::TVarEntryInfo& ent) override;
  int resolveBinding(EShLanguage stage, glslang::TVarEntryInfo& ent) override;
  int resolveSet(EShLanguage stage, glslang::TVarEntryInfo& ent) override;
  int resolveUniformLocation(EShLanguage stage,
                             glslang::TVarEntryInfo& ent) override;
  bool validateInOut(EShLanguage stage, glslang::TVarEntryInfo& ent) override;
  int resolveInOutLocation(EShLanguage stage,
                           glslang::TVarEntryInfo& ent) override;
  int resolveInOutComponent(EShLanguage stage,
                            glslang::TVarEntryInfo& ent) override;
  int resolveInOutIndex(EShLanguage stage,
                        glslang::TVarEntryInfo& ent) override;
  void notifyBinding(EShLanguage stage, glslang::TVarEntryInfo& ent) override;
  void notifyInOut(EShLanguage stage, glslang::TVarEntryInfo& ent) override;
  void beginNotifications(EShLanguage stage) override;
  void endNotifications(EShLanguage stage) override;
  void beginResolve(EShLanguage stage) override;
  void endResolve(EShLanguage stage) override;
  void beginCollect(EShLanguage stage) override;
  void endCollect(EShLanguage stage) override;
  void reserverStorageSlot(glslang::TVarEntryInfo& ent,
                           TInfoSink& infoSink) override;
  void reserverResourceSlot(glslang::TVarEntryInfo& ent,
                            TInfoSink& infoSink) override;
  void addStage(EShLanguage stage,
                glslang::TIntermediate& intermediate) override;

 private:
  // How a reserved slot came to be occupied.  This decides whether a collision
  // is a reportable conflict.
  enum class Owner {
    kExplicit,  // Pinned by a `binding` or `set` layout in the shader.
    kLayout,    // Reserved by the shared resource layout.
    kAutomatic  // Assigned by automatic binding.
  };

  // One declared resource in the stage.
  struct Resource {
    glslang::TSourceLoc loc;
    ResourceType type = ResourceType::kOther;
    bool live = false;
    // For an interface block, glslang reports the block type name from
    // getAccessName() and the instance name from getName().  A shared layout
    // entry may match either, so both are kept.  |alt_name| is empty when it
    // equals the access name.
    std::string alt_name;
    // The descriptor set glslang's default rules choose when the shader does
    // not assign one.
    int default_set = 0;
    bool has_explicit_set = false;
    int explicit_set = 0;
    // True when the source has a `binding` layout qualifier.  The effective
    // binding includes the automatic binding base.
    bool has_explicit_binding = false;
    int explicit_binding = 0;
    // Number of consecutive bindings the declaration consumes.
    int binding_count = 1;
    // True when the resource matches a shared layout entry by name and kind.
    bool has_entry = false;
    int entry_set = 0;
    int entry_binding = 0;
    // The layout entry the resource matched, or null.  Used so an explicit
    // binding that agrees with its own entry is not counted as a collision.
    const Compiler::ResourceLayoutEntry* matched_entry = nullptr;
    // The final set and binding, or -1 in binding when none has been decided.
    int final_set = 0;
    int final_binding = -1;
  };

  // A reserved (set, binding) slot and who reserved it.
  struct Occupant {
    Owner owner = Owner::kAutomatic;
    // Human-readable description of the reserving side, used in conflict
    // messages.  Empty for automatic reservations.
    std::string description;
  };

  // Records a resource during the notification pass.
  void RecordResource(EShLanguage stage, glslang::TVarEntryInfo& ent);
  // Matches resources to entries and reserves every fixed slot for a stage.
  void PlanStage(EShLanguage stage);
  // Reserves [slot, slot+size) under |occupant|, reporting a conflict when the
  // new reservation collides with a different resource's fixed reservation.
  void Occupy(EShLanguage stage, int namespace_key, int set, int slot, int size,
              const Occupant& occupant, const glslang::TSourceLoc& loc);
  // Finds and reserves the lowest run of |size| free bindings at or above
  // |base|.
  int Allocate(EShLanguage stage, int namespace_key, int set, uint32_t base,
               int size);
  // Returns the automatic binding base for a resource type.
  uint32_t BaseBinding(ResourceType type) const;
  // Number of consecutive bindings a declaration consumes.
  int BindingCount(const glslang::TType& type) const;
  // Returns the binding namespace for a resource type.
  int NamespaceKey(ResourceType type) const;
  // Returns the resource types a layout entry of the given kind can match.
  static std::vector<ResourceType> TypesForKind(Compiler::UniformKind kind);
  // True when a resource in |stage| has an explicit binding that agrees with
  // |entry| for the given namespace, so the entry's slot is already reserved.
  bool IsCoveredByAgreeingExplicit(EShLanguage stage,
                                   const Compiler::ResourceLayoutEntry& entry,
                                   int namespace_key) const;
  // Returns the resource for |stage| with the given access name, or null.
  Resource* FindResource(EShLanguage stage, const std::string& name);
  // Appends |message| unless an identical conflict is already recorded.
  void AddConflict(const glslang::TSourceLoc& loc, const std::string& message);

  std::unique_ptr<glslang::TIoMapResolver> delegate_;
  std::vector<Compiler::ResourceLayoutEntry> entries_;
  const uint32_t* auto_binding_base_;
  bool per_resource_type_namespaces_;
  std::vector<Conflict> conflicts_;

  std::map<EShLanguage, std::map<std::string, Resource>> resources_;
  // The order in which access names were first seen per stage, so that
  // automatic allocation follows source order within a stage.
  std::map<EShLanguage, std::vector<std::string>> order_;
  // Occupied slots per stage, keyed by (namespace, set) then slot.
  using SlotMap = std::map<int, Occupant>;
  using NamespaceMap = std::map<std::pair<int, int>, SlotMap>;
  std::map<EShLanguage, NamespaceMap> occupied_;
  std::map<EShLanguage, bool> planned_;

  Resolver(const Resolver&) = delete;
  Resolver& operator=(const Resolver&) = delete;
};

}  // namespace resource_layout
}  // namespace shaderc_util

#endif  // LIBSHADERC_UTIL_SRC_RESOURCE_LAYOUT_H_
