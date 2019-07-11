#include "graphics/model.h"
#include "graphics/buffer.h"
#include "graphics/graphics.h"
#include "graphics/material.h"
#include "graphics/mesh.h"
#include "graphics/texture.h"
#include "resources/shaders.h"
#include "core/maf.h"
#include "core/ref.h"
#include <stdlib.h>
#include <float.h>
#include <math.h>

typedef struct {
  float properties[3][4];
} NodeTransform;

struct Model {
  struct ModelData* data;
  struct Buffer** buffers;
  struct Mesh** meshes;
  struct Texture** textures;
  struct Material** materials;
  struct Material* userMaterial;
  NodeTransform* localTransforms;
  float* globalTransforms;
  bool transformsDirty;
};

static void updateGlobalTransform(Model* model, uint32_t nodeIndex, mat4 parent) {
  ModelNode* node = &model->data->nodes[nodeIndex];
  mat4 global = model->globalTransforms + 16 * nodeIndex;
  mat4_init(global, parent);

  if (node->matrix) {
    mat4_multiply(global, node->transform);
  } else {
    NodeTransform* local = &model->localTransforms[nodeIndex];
    vec3 T = local->properties[PROP_TRANSLATION];
    quat R = local->properties[PROP_ROTATION];
    vec3 S = local->properties[PROP_SCALE];
    mat4_translate(global, T[0], T[1], T[2]);
    mat4_rotateQuat(global, R);
    mat4_scale(global, S[0], S[1], S[2]);
  }

  for (uint32_t i = 0; i < node->childCount; i++) {
    updateGlobalTransform(model, node->children[i], global);
  }
}

static void renderNode(Model* model, uint32_t nodeIndex, uint32_t instances) {
  ModelNode* node = &model->data->nodes[nodeIndex];
  mat4 globalTransform = model->globalTransforms + 16 * nodeIndex;

  if (node->primitiveCount > 0) {
    bool animated = node->skin != ~0u;
    float pose[16 * MAX_BONES];

    if (animated) {
      ModelSkin* skin = &model->data->skins[node->skin];

      for (uint32_t j = 0; j < skin->jointCount; j++) {
        mat4 globalJointTransform = model->globalTransforms + 16 * skin->joints[j];
        mat4 inverseBindMatrix = skin->inverseBindMatrices + 16 * j;
        mat4 jointPose = pose + 16 * j;

        mat4_set(jointPose, globalTransform);
        mat4_invert(jointPose);
        mat4_multiply(jointPose, globalJointTransform);
        mat4_multiply(jointPose, inverseBindMatrix);
      }
    }

    for (uint32_t i = 0; i < node->primitiveCount; i++) {
      ModelPrimitive* primitive = &model->data->primitives[node->primitiveIndex + i];
      Mesh* mesh = model->meshes[node->primitiveIndex + i];
      Material* material = primitive->material == ~0u ? NULL : model->materials[primitive->material];
      lovrMeshSetMaterial(mesh, model->userMaterial ? model->userMaterial : material);
      lovrMeshSetDrawMode(mesh, primitive->mode);
      lovrGraphicsDrawMesh(mesh, globalTransform, instances, animated ? pose : NULL);
    }
  }

  for (uint32_t i = 0; i < node->childCount; i++) {
    renderNode(model, node->children[i], instances);
  }
}

Model* lovrModelCreate(ModelData* data) {
  Model* model = lovrAlloc(Model);
  model->data = data;
  lovrRetain(data);

  // Geometry
  if (data->primitiveCount > 0) {
    if (data->bufferCount > 0) {
      model->buffers = calloc(data->bufferCount, sizeof(Buffer*));
    }

    model->meshes = calloc(data->primitiveCount, sizeof(Mesh*));
    for (uint32_t i = 0; i < data->primitiveCount; i++) {
      ModelPrimitive* primitive = &data->primitives[i];
      model->meshes[i] = lovrMeshCreate(primitive->mode, NULL, 0);

      bool setDrawRange = false;
      for (uint32_t j = 0; j < MAX_DEFAULT_ATTRIBUTES; j++) {
        if (primitive->attributes[j]) {
          ModelAttribute* attribute = primitive->attributes[j];

          if (!model->buffers[attribute->buffer]) {
            ModelBuffer* buffer = &data->buffers[attribute->buffer];
            model->buffers[attribute->buffer] = lovrBufferCreate(buffer->size, buffer->data, BUFFER_VERTEX, USAGE_STATIC, false);
          }

          lovrMeshAttachAttribute(model->meshes[i], lovrShaderAttributeNames[j], &(MeshAttribute) {
            .buffer = model->buffers[attribute->buffer],
            .offset = attribute->offset,
            .stride = data->buffers[attribute->buffer].stride,
            .type = attribute->type,
            .components = attribute->components,
            .integer = j == ATTR_BONES,
            .normalized = attribute->normalized
          });

          if (!setDrawRange && !primitive->indices) {
            lovrMeshSetDrawRange(model->meshes[i], 0, attribute->count);
            setDrawRange = true;
          }
        }
      }

      lovrMeshAttachAttribute(model->meshes[i], "lovrDrawID", &(MeshAttribute) {
        .buffer = lovrGraphicsGetIdentityBuffer(),
        .type = U8,
        .components = 1,
        .divisor = 1,
        .integer = true
      });

      if (primitive->indices) {
        ModelAttribute* attribute = primitive->indices;

        if (!model->buffers[attribute->buffer]) {
          ModelBuffer* buffer = &data->buffers[attribute->buffer];
          model->buffers[attribute->buffer] = lovrBufferCreate(buffer->size, buffer->data, BUFFER_INDEX, USAGE_STATIC, false);
        }

        size_t indexSize = attribute->type == U16 ? 2 : 4;
        lovrMeshSetIndexBuffer(model->meshes[i], model->buffers[attribute->buffer], attribute->count, indexSize, attribute->offset);
        lovrMeshSetDrawRange(model->meshes[i], 0, attribute->count);
      }
    }
  }

  // Materials
  if (data->materialCount > 0) {
    model->materials = malloc(data->materialCount * sizeof(Material*));

    if (data->textureCount > 0) {
      model->textures = calloc(data->textureCount, sizeof(Texture*));
    }

    for (uint32_t i = 0; i < data->materialCount; i++) {
      Material* material = lovrMaterialCreate();

      for (uint32_t j = 0; j < MAX_MATERIAL_SCALARS; j++) {
        lovrMaterialSetScalar(material, j, data->materials[i].scalars[j]);
      }

      for (uint32_t j = 0; j < MAX_MATERIAL_COLORS; j++) {
        lovrMaterialSetColor(material, j, data->materials[i].colors[j]);
      }

      for (uint32_t j = 0; j < MAX_MATERIAL_TEXTURES; j++) {
        uint32_t index = data->materials[i].textures[j];

        if (index != ~0u) {
          if (!model->textures[index]) {
            TextureData* textureData = data->textures[index];
            bool srgb = j == TEXTURE_DIFFUSE || j == TEXTURE_EMISSIVE;
            model->textures[index] = lovrTextureCreate(TEXTURE_2D, &textureData, 1, srgb, true, 0);
            lovrTextureSetFilter(model->textures[index], data->materials[i].filters[j]);
            lovrTextureSetWrap(model->textures[index], data->materials[i].wraps[j]);
          }

          lovrMaterialSetTexture(material, j, model->textures[index]);
        }
      }

      model->materials[i] = material;
    }
  }

  model->localTransforms = malloc(sizeof(NodeTransform) * data->nodeCount);
  model->globalTransforms = malloc(16 * sizeof(float) * data->nodeCount);
  model->transformsDirty = true;

  for (uint32_t i = 0; i < data->nodeCount; i++) {
    if (data->nodes[i].matrix) {
      vec3_set(model->localTransforms[i].properties[PROP_TRANSLATION], 0.f, 0.f, 0.f);
      quat_set(model->localTransforms[i].properties[PROP_ROTATION], 0.f, 0.f, 0.f, 1.f);
      vec3_set(model->localTransforms[i].properties[PROP_SCALE], 1.f, 1.f, 1.f);
    } else {
      vec3_init(model->localTransforms[i].properties[PROP_TRANSLATION], data->nodes[i].translation);
      quat_init(model->localTransforms[i].properties[PROP_ROTATION], data->nodes[i].rotation);
      vec3_init(model->localTransforms[i].properties[PROP_SCALE], data->nodes[i].scale);
    }
  }

  return model;
}

void lovrModelDestroy(void* ref) {
  Model* model = ref;
  for (uint32_t i = 0; i < model->data->bufferCount; i++) {
    lovrRelease(Buffer, model->buffers[i]);
  }
  for (uint32_t i = 0; i < model->data->primitiveCount; i++) {
    lovrRelease(Mesh, model->meshes[i]);
  }
  for (uint32_t i = 0; i < model->data->textureCount; i++) {
    lovrRelease(Texture, model->textures[i]);
  }
  for (uint32_t i = 0; i < model->data->materialCount; i++) {
    lovrRelease(Material, model->materials[i]);
  }
  lovrRelease(Material, model->userMaterial);
  lovrRelease(ModelData, model->data);
  free(model->globalTransforms);
  free(model->localTransforms);
}

ModelData* lovrModelGetModelData(Model* model) {
  return model->data;
}

void lovrModelDraw(Model* model, mat4 transform, uint32_t instances) {
  if (model->transformsDirty) {
    updateGlobalTransform(model, model->data->rootNode, (float[]) MAT4_IDENTITY);
    model->transformsDirty = false;
  }

  lovrGraphicsPush();
  lovrGraphicsMatrixTransform(transform);
  renderNode(model, model->data->rootNode, instances);
  lovrGraphicsPop();
}

void lovrModelAnimate(Model* model, uint32_t animationIndex, float time, float alpha) {
  if (alpha <= 0.f) {
    return;
  }

  lovrAssert(animationIndex < model->data->animationCount, "Invalid animation index #%d (Model only has %d animations)", animationIndex, model->data->animationCount);
  ModelAnimation* animation = &model->data->animations[animationIndex];
  time = fmodf(time, animation->duration);

  for (uint32_t i = 0; i < animation->channelCount; i++) {
    ModelAnimationChannel* channel = &animation->channels[i];
    uint32_t nodeIndex = channel->nodeIndex;
    NodeTransform* transform = &model->localTransforms[nodeIndex];

    uint32_t keyframe = 0;
    while (keyframe < channel->keyframeCount && channel->times[keyframe] < time) {
      keyframe++;
    }

    float property[4];
    bool rotate = channel->property == PROP_ROTATION;
    size_t n = 3 + rotate;
    float* (*lerp)(float* a, float* b, float t) = rotate ? quat_slerp : vec3_lerp;

    if (keyframe >= channel->keyframeCount) {
      memcpy(property, channel->data + CLAMP(keyframe, 0, channel->keyframeCount - 1) * n, n * sizeof(float));
    } else {
      float t1 = channel->times[keyframe - 1];
      float t2 = channel->times[keyframe];
      float z = (time - t1) / (t2 - t1);

      switch (channel->smoothing) {
        case SMOOTH_STEP:
          memcpy(property, channel->data + (z >= .5f ? keyframe : keyframe - 1) * n, n * sizeof(float));
          break;
        case SMOOTH_LINEAR:
          memcpy(property, channel->data + (keyframe - 1) * n, n * sizeof(float));
          lerp(property, channel->data + keyframe * n, z);
          break;
        case SMOOTH_CUBIC: {
          int stride = 3 * n;
          float* p0 = channel->data + (keyframe - 1) * stride + 1 * n;
          float* m0 = channel->data + (keyframe - 1) * stride + 2 * n;
          float* p1 = channel->data + (keyframe - 0) * stride + 1 * n;
          float* m1 = channel->data + (keyframe - 0) * stride + 0 * n;
          float dt = t2 - t1;
          float z2 = z * z;
          float z3 = z2 * z;
          float a = 2.f * z3 - 3.f * z2 + 1.f;
          float b = 2.f * z3 - 3.f * z2 + 1.f;
          float c = (-2.f * z3 + 3.f * z2);
          float d = (z3 * -z2) * dt;
          for (size_t j = 0; j < n; j++) {
            property[j] = a * p0[j] + b * m0[j] + c * p1[j] + d * m1[j];
          }
          break;
        }
        default:
          break;
      }
    }

    if (alpha >= 1.f) {
      memcpy(transform->properties[channel->property], property, n * sizeof(float));
    } else {
      lerp(transform->properties[channel->property], property, alpha);
    }
  }

  model->transformsDirty = true;
}

Material* lovrModelGetMaterial(Model* model) {
  return model->userMaterial;
}

void lovrModelSetMaterial(Model* model, Material* material) {
  lovrRetain(material);
  lovrRelease(Material, model->userMaterial);
  model->userMaterial = material;
}

static void applyAABB(Model* model, uint32_t nodeIndex, float aabb[6]) {
  ModelNode* node = &model->data->nodes[nodeIndex];

  for (uint32_t i = 0; i < node->primitiveCount; i++) {
    ModelAttribute* position = model->data->primitives[node->primitiveIndex + i].attributes[ATTR_POSITION];
    if (position && position->hasMin && position->hasMax) {
      mat4 m = model->globalTransforms + 16 * nodeIndex;

      float xa[3] = { position->min[0] * m[0], position->min[0] * m[1], position->min[0] * m[2] };
      float xb[3] = { position->max[0] * m[0], position->max[0] * m[1], position->max[0] * m[2] };

      float ya[3] = { position->min[1] * m[4], position->min[1] * m[5], position->min[1] * m[6] };
      float yb[3] = { position->max[1] * m[4], position->max[1] * m[5], position->max[1] * m[6] };

      float za[3] = { position->min[2] * m[8], position->min[2] * m[9], position->min[2] * m[10] };
      float zb[3] = { position->max[2] * m[8], position->max[2] * m[9], position->max[2] * m[10] };

      float min[3] = {
        MIN(xa[0], xb[0]) + MIN(ya[0], yb[0]) + MIN(za[0], zb[0]) + m[12],
        MIN(xa[1], xb[1]) + MIN(ya[1], yb[1]) + MIN(za[1], zb[1]) + m[13],
        MIN(xa[2], xb[2]) + MIN(ya[2], yb[2]) + MIN(za[2], zb[2]) + m[14]
      };

      float max[3] = {
        MAX(xa[0], xb[0]) + MAX(ya[0], yb[0]) + MAX(za[0], zb[0]) + m[12],
        MAX(xa[1], xb[1]) + MAX(ya[1], yb[1]) + MAX(za[1], zb[1]) + m[13],
        MAX(xa[2], xb[2]) + MAX(ya[2], yb[2]) + MAX(za[2], zb[2]) + m[14]
      };

      aabb[0] = MIN(aabb[0], min[0]);
      aabb[1] = MAX(aabb[1], max[0]);
      aabb[2] = MIN(aabb[2], min[1]);
      aabb[3] = MAX(aabb[3], max[1]);
      aabb[4] = MIN(aabb[4], min[2]);
      aabb[5] = MAX(aabb[5], max[2]);
    }
  }

  for (uint32_t i = 0; i < node->childCount; i++) {
    applyAABB(model, node->children[i], aabb);
  }
}

void lovrModelGetAABB(Model* model, float aabb[6]) {
  if (model->transformsDirty) {
    updateGlobalTransform(model, model->data->rootNode, (float[]) MAT4_IDENTITY);
    model->transformsDirty = false;
  }

  aabb[0] = aabb[2] = aabb[4] = FLT_MAX;
  aabb[1] = aabb[3] = aabb[5] = -FLT_MAX;
  applyAABB(model, model->data->rootNode, aabb);
}
