#pragma once

#include "safe.hpp"

namespace usa1to1::core {

// A streaming status of "loaded" is not evidence that either resource is
// present. These checks also work with host test doubles of the SDK structs.
template <class ModelInfo> bool HasMappedModelResources(const ModelInfo *model) {
    return IsMappedRange(model, sizeof(*model)) &&
           IsMappedRange(model->m_pRwObject, sizeof(*model->m_pRwObject)) &&
           IsMappedRange(model->m_pColModel, sizeof(*model->m_pColModel));
}

template <class Object> bool HasMappedObjectInstance(const Object *object) {
    return IsMappedRange(object, sizeof(*object)) &&
           IsMappedRange(object->m_pRwObject, sizeof(*object->m_pRwObject));
}

} // namespace usa1to1::core
