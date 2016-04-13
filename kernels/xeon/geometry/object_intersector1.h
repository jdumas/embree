// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "object.h"
#include "../../common/ray.h"

namespace embree
{
  namespace isa
  {
    struct ObjectIntersector1
    {
      typedef Object Primitive;
      
      struct Precalculations {
        __forceinline Precalculations() {}
        __forceinline Precalculations (const Ray& ray, const void *ptr) {}
      };
      
      static __forceinline void intersect(const Precalculations& pre, Ray& ray, const Primitive& prim, Scene* scene, const unsigned* geomID_to_instID) 
      {
        AVX_ZERO_UPPER();
        AccelSet* accel = (AccelSet*) scene->get(prim.geomID);

        /* perform ray mask test */
#if defined(RTCORE_RAY_MASK)
        if ((ray.mask & accel->mask) == 0) 
          return;
#endif

        accel->intersect((RTCRay&)ray,prim.primID);
      }
      
      static __forceinline bool occluded(const Precalculations& pre, Ray& ray, const Primitive& prim, Scene* scene, const unsigned* geomID_to_instID) 
      {
        AVX_ZERO_UPPER();
        AccelSet* accel = (AccelSet*) scene->get(prim.geomID);

        /* perform ray mask test */
#if defined(RTCORE_RAY_MASK)
        if ((ray.mask & accel->mask) == 0) 
          return false;
#endif

        accel->occluded((RTCRay&)ray,prim.primID);
        return ray.geomID == 0;
      }
      
      template<typename Context>
      static __forceinline void intersect(Precalculations* pre, size_t valid_in, Ray** rays, Context* ctx, size_t ty, const Primitive* prims, size_t num, Scene* scene, const unsigned* geomID_to_instID, size_t& lazy_node)
      {
        AVX_ZERO_UPPER();
        
        /* intersect all primitives */
        for (size_t i=0; i<num; i++)
        {
          const Primitive& prim = prims[i];
          AccelSet* accel = (AccelSet*) scene->get(prim.geomID);

          size_t N = 0, valid = valid_in;
          Ray* rays_filtered[64];
          while (unlikely(valid)) 
          {
            const size_t i = __bscf(valid);
            Ray* ray = rays[i];

            /* perform ray mask test */
#if defined(RTCORE_RAY_MASK)
            if ((ray->mask & accel->mask) == 0) 
              continue;
#endif
            rays_filtered[N++] = ray;
          }
          if (unlikely(N == 0)) continue;

          /* call user stream intersection function */
          accel->intersect1N((RTCRay**)rays_filtered,N,prim.primID);
        }

        /* update all contexts */
        size_t valid = valid_in;
        while (unlikely(valid)) {
          const size_t i = __bscf(valid);
          ctx[i].update(rays[i]);
        }
      }

      static __forceinline size_t occluded(Precalculations* pre, size_t valid_in, Ray** rays, size_t ty, const Primitive* prims, size_t num, Scene* scene, const unsigned* geomID_to_instID, size_t& lazy_node)
      {
        AVX_ZERO_UPPER();
        size_t hit = 0;
        
        /* intersect all primitives */
        for (size_t i=0; i<num; i++)
        {
          const Primitive& prim = prims[i];
          AccelSet* accel = (AccelSet*) scene->get(prim.geomID);

          size_t N = 0, valid = valid_in;
          Ray* rays_filtered[64];
          size_t index_filtered[64];
          while (unlikely(valid)) 
          {
            const size_t i = __bscf(valid);
            Ray* ray = rays[i];

            /* perform ray mask test */
#if defined(RTCORE_RAY_MASK)
            if ((ray->mask & accel->mask) == 0) 
              continue;
#endif
            rays_filtered[N] = ray;
            index_filtered[N] = i;
            N++;
          }
          if (unlikely(N == 0)) continue;

          /* call user stream occluded function */
          accel->occluded1N((RTCRay**)rays_filtered,N,prim.primID);

          /* mark occluded rays */
          for (size_t i=0; i<N; i++)
          {
            if (rays_filtered[i]->geomID == 0) {
              hit |= (size_t)1 << index_filtered[i];
            }
          }
          valid_in &= ~hit;
        }
        return hit;
      }
    };
  }
}
