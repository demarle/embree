// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
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

#include "bvh.h"
#include "../common/ray.h"
#include "../common/stack_item.h"

namespace embree
{
  namespace isa 
  {
    /*! BVH hybrid packet intersector. Switches between packet and single ray traversal (optional). */
    template<int N, int K, int types, bool robust, typename PrimitiveIntersectorK, bool single = true>
    class BVHNIntersectorKHybrid
    {
      /* right now AVX512KNL SIMD extension only for standard node types */
      static const size_t Nx = types == BVH_AN1 ? vextend<N>::size : N;

      /* shortcuts for frequently used types */
      typedef typename PrimitiveIntersectorK::Precalculations Precalculations;
      typedef typename PrimitiveIntersectorK::Primitive Primitive;
      typedef BVHN<N> BVH;
      typedef typename BVH::NodeRef NodeRef;
      typedef typename BVH::BaseNode BaseNode;
      typedef typename BVH::AlignedNode AlignedNode;
      typedef typename BVH::AlignedNodeMB AlignedNodeMB;
      
      static const size_t stackSizeSingle = 1+(N-1)*BVH::maxDepth+3; // +3 due to 16-wide store
      static const size_t stackSizeChunk = 1+(N-1)*BVH::maxDepth;

      static const size_t switchThresholdIncoherent = \
      (K==4)  ? 3 :
      (K==8)  ? ((N==4) ? 5 : 7) :
      (K==16) ? 14 : // 14 seems to work best for KNL due to better ordered chunk traversal
      0;


      /* Optimized frustum test. We calculate t=(p-org)/dir in ray/box
       * intersection. We assume the rays are split by octant, thus
       * dir intervals are either positive or negative in each
       * dimension.

         Case 1: dir.min >= 0 && dir.max >= 0:
           t_min = (p_min - org_max) / dir_max = (p_min - org_max)*rdir_min = p_min*rdir_min - org_max*rdir_min
           t_max = (p_max - org_min) / dir_min = (p_max - org_min)*rdir_max = p_max*rdir_max - org_min*rdir_max

         Case 2: dir.min < 0 && dir.max < 0:
           t_min = (p_max - org_min) / dir_min = (p_max - org_min)*rdir_max = p_max*rdir_max - org_min*rdir_max
           t_max = (p_min - org_max) / dir_max = (p_min - org_max)*rdir_min = p_min*rdir_min - org_max*rdir_min
      */
      
      struct Frustum
      {
        __forceinline Frustum(const vbool<K>  &valid,
                              const Vec3vf<K> &org,
                              const Vec3vf<K> &rdir,
                              const vfloat<K> &ray_tnear,
                              const vfloat<K> &ray_tfar)
        {
          const Vec3fa reduced_min_org( reduce_min(select(valid,org.x,pos_inf)),
                                        reduce_min(select(valid,org.y,pos_inf)),
                                        reduce_min(select(valid,org.z,pos_inf)) );
          const Vec3fa reduced_max_org( reduce_max(select(valid,org.x,neg_inf)),
                                        reduce_max(select(valid,org.y,neg_inf)),
                                        reduce_max(select(valid,org.z,neg_inf)) );
          
          const Vec3fa reduced_min_rdir( reduce_min(select(valid,rdir.x,pos_inf)),
                                         reduce_min(select(valid,rdir.y,pos_inf)),
                                         reduce_min(select(valid,rdir.z,pos_inf)) );
          const Vec3fa reduced_max_rdir( reduce_max(select(valid,rdir.x,neg_inf)),
                                         reduce_max(select(valid,rdir.y,neg_inf)),
                                         reduce_max(select(valid,rdir.z,neg_inf)) );

          min_rdir = select(ge_mask(reduced_min_rdir, Vec3fa(zero)), reduced_min_rdir, reduced_max_rdir);
          max_rdir = select(ge_mask(reduced_min_rdir, Vec3fa(zero)), reduced_max_rdir, reduced_min_rdir);

          if (!robust)
          {
            max_org_min_rdir = min_rdir * select(ge_mask(reduced_min_rdir, Vec3fa(zero)), reduced_max_org, reduced_min_org);
            min_org_max_rdir = max_rdir * select(ge_mask(reduced_min_rdir, Vec3fa(zero)), reduced_min_org, reduced_max_org);
          }
          else
          {
            max_org_min_rdir = select(ge_mask(reduced_min_rdir, Vec3fa(zero)), reduced_max_org, reduced_min_org);
            min_org_max_rdir = select(ge_mask(reduced_min_rdir, Vec3fa(zero)), reduced_min_org, reduced_max_org);
          }

          min_dist = reduce_min(select(valid,ray_tnear,vfloat<K>(pos_inf)));
          max_dist = reduce_max(select(valid,ray_tfar ,vfloat<K>(neg_inf)));

#if defined(__AVX512ER__) // KNL+
          minmax_rdirX = align_shift_right<16/2>(vfloat16(max_rdir.x),vfloat16(min_rdir.x));
          minmax_rdirY = align_shift_right<16/2>(vfloat16(max_rdir.y),vfloat16(min_rdir.y));
          minmax_rdirZ = align_shift_right<16/2>(vfloat16(max_rdir.z),vfloat16(min_rdir.z));

          minmax_org_maxmin_rdirX = align_shift_right<16/2>(vfloat16(min_org_max_rdir.x),vfloat16(max_org_min_rdir.x));
          minmax_org_maxmin_rdirY = align_shift_right<16/2>(vfloat16(min_org_max_rdir.y),vfloat16(max_org_min_rdir.y));
          minmax_org_maxmin_rdirZ = align_shift_right<16/2>(vfloat16(min_org_max_rdir.z),vfloat16(max_org_min_rdir.z));

          maskX = (vfloat<16>(min_rdir.x) >= 0.0f) ^ vbool16(0xff00);
          maskY = (vfloat<16>(min_rdir.y) >= 0.0f) ^ vbool16(0xff00);
          maskZ = (vfloat<16>(min_rdir.z) >= 0.0f) ^ vbool16(0xff00);
#else
          nearX = (min_rdir.x < 0.0f) ? 1*sizeof(vfloat<N>) : 0*sizeof(vfloat<N>);
          nearY = (min_rdir.y < 0.0f) ? 3*sizeof(vfloat<N>) : 2*sizeof(vfloat<N>);
          nearZ = (min_rdir.z < 0.0f) ? 5*sizeof(vfloat<N>) : 4*sizeof(vfloat<N>);
          farX  = nearX ^ sizeof(vfloat<N>);
          farY  = nearY ^ sizeof(vfloat<N>);
          farZ  = nearZ ^ sizeof(vfloat<N>);
#endif
        }

        __forceinline unsigned int intersect(const NodeRef &nodeRef, float * const __restrict__ dist) const
        {
          /* only default alignedNodes are currently supported */
          const AlignedNode* __restrict__ const node = nodeRef.alignedNode();
          
#if defined(__AVX512ER__)

          const vfloat16 lowerX = vfloat16(*(const vfloat<N>*)((const char*)&node->lower_x));
          const vfloat16 lowerY = vfloat16(*(const vfloat<N>*)((const char*)&node->lower_y));
          const vfloat16 lowerZ = vfloat16(*(const vfloat<N>*)((const char*)&node->lower_z));
          const vfloat16 upperX = vfloat16(*(const vfloat<N>*)((const char*)&node->upper_x));
          const vfloat16 upperY = vfloat16(*(const vfloat<N>*)((const char*)&node->upper_y));
          const vfloat16 upperZ = vfloat16(*(const vfloat<N>*)((const char*)&node->upper_z));
          
          const vfloat16 bminmaxX = select(maskX,lowerX,upperX);
          const vfloat16 bminmaxY = select(maskY,lowerY,upperY);
          const vfloat16 bminmaxZ = select(maskZ,lowerZ,upperZ);

          if (robust)
          {
             const vfloat16 fminmaxX = (bminmaxX - minmax_org_maxmin_rdirX) * minmax_rdirX; 
             const vfloat16 fminmaxY = (bminmaxY - minmax_org_maxmin_rdirY) * minmax_rdirY; 
             const vfloat16 fminmaxZ = (bminmaxZ - minmax_org_maxmin_rdirZ) * minmax_rdirZ; 
             const float round_down = 1.0f-2.0f*float(ulp); // FIXME: use per instruction rounding for AVX512 
             const float round_up   = 1.0f+2.0f*float(ulp); 
             const vfloat16 fmin  = round_down*max(fminmaxX, fminmaxY, fminmaxZ, vfloat16(min_dist));  
             vfloat16::store(dist,fmin); 
             const vfloat16 fmax  = round_up*min(fminmaxX, fminmaxY, fminmaxZ, vfloat16(max_dist)); 
             const vbool16 vmask_node_hit = le(vbool16((((int)1 << N)-1)),fmin,align_shift_right<8>(fmax,fmax)); 
             return movemask(vmask_node_hit); 
          }
          else
          {
            const vfloat16 fminmaxX = msub(bminmaxX, minmax_rdirX, minmax_org_maxmin_rdirX);
            const vfloat16 fminmaxY = msub(bminmaxY, minmax_rdirY, minmax_org_maxmin_rdirY);
            const vfloat16 fminmaxZ = msub(bminmaxZ, minmax_rdirZ, minmax_org_maxmin_rdirZ);
            
            const vfloat16 fmin  = max(fminmaxX, fminmaxY, fminmaxZ, vfloat16(min_dist)); 
            vfloat16::store(dist,fmin);
            const vfloat16 fmax  = min(fminmaxX, fminmaxY, fminmaxZ, vfloat16(max_dist));
            const vbool16 vmask_node_hit = le(vbool16((((int)1 << N)-1)),fmin,align_shift_right<8>(fmax,fmax));
            return movemask(vmask_node_hit);
          }
#else
          const vfloat<N> bminX = *(const vfloat<N>*)((const char*)&node->lower_x + nearX);
          const vfloat<N> bminY = *(const vfloat<N>*)((const char*)&node->lower_x + nearY);
          const vfloat<N> bminZ = *(const vfloat<N>*)((const char*)&node->lower_x + nearZ);
          const vfloat<N> bmaxX = *(const vfloat<N>*)((const char*)&node->lower_x + farX);
          const vfloat<N> bmaxY = *(const vfloat<N>*)((const char*)&node->lower_x + farY);
          const vfloat<N> bmaxZ = *(const vfloat<N>*)((const char*)&node->lower_x + farZ);
                    
          if (robust)
          {
            const vfloat<N> fminX = (bminX - vfloat<N>(max_org_min_rdir.x)) * vfloat<N>(min_rdir.x);
            const vfloat<N> fminY = (bminY - vfloat<N>(max_org_min_rdir.y)) * vfloat<N>(min_rdir.y);
            const vfloat<N> fminZ = (bminZ - vfloat<N>(max_org_min_rdir.z)) * vfloat<N>(min_rdir.z);
            const vfloat<N> fmaxX = (bmaxX - vfloat<N>(min_org_max_rdir.x)) * vfloat<N>(max_rdir.x);
            const vfloat<N> fmaxY = (bmaxY - vfloat<N>(min_org_max_rdir.y)) * vfloat<N>(max_rdir.y);
            const vfloat<N> fmaxZ = (bmaxZ - vfloat<N>(min_org_max_rdir.z)) * vfloat<N>(max_rdir.z);

            const float round_down = 1.0f-2.0f*float(ulp); // FIXME: use per instruction rounding for AVX512
            const float round_up   = 1.0f+2.0f*float(ulp);
            const vfloat<N> fmin  = max(fminX, fminY, fminZ, vfloat<N>(min_dist)); 
            vfloat<N>::store(dist,fmin);
            const vfloat<N> fmax  = min(fmaxX, fmaxY, fmaxZ, vfloat<N>(max_dist));
            const vbool<N> vmask_node_hit = (round_down*fmin <= round_up*fmax);
            size_t m_node = movemask(vmask_node_hit) & (((size_t)1 << N)-1);
            return m_node;          
          }
          else
          {
            const vfloat<N> fminX = msub(bminX, vfloat<N>(min_rdir.x), vfloat<N>(max_org_min_rdir.x));
            const vfloat<N> fminY = msub(bminY, vfloat<N>(min_rdir.y), vfloat<N>(max_org_min_rdir.y));
            const vfloat<N> fminZ = msub(bminZ, vfloat<N>(min_rdir.z), vfloat<N>(max_org_min_rdir.z));
            const vfloat<N> fmaxX = msub(bmaxX, vfloat<N>(max_rdir.x), vfloat<N>(min_org_max_rdir.x));
            const vfloat<N> fmaxY = msub(bmaxY, vfloat<N>(max_rdir.y), vfloat<N>(min_org_max_rdir.y));
            const vfloat<N> fmaxZ = msub(bmaxZ, vfloat<N>(max_rdir.z), vfloat<N>(min_org_max_rdir.z));

            const vfloat<N> fmin  = maxi(fminX, fminY, fminZ, vfloat<N>(min_dist)); 
            vfloat<N>::store(dist,fmin);
            const vfloat<N> fmax  = mini(fmaxX, fmaxY, fmaxZ, vfloat<N>(max_dist));
            const vbool<N> vmask_node_hit = fmin <= fmax;
            size_t m_node = movemask(vmask_node_hit) & (((size_t)1 << N)-1);
            return m_node;          
          }
#endif
        }

        __forceinline void updateMaxDist(const vfloat<K> &ray_tfar)
        {
          max_dist = reduce_max(ray_tfar);
        }

#if defined(__AVX512ER__) // KNL+
        vbool16 maskX, maskY, maskZ;
        vfloat16 minmax_rdirX, minmax_rdirY, minmax_rdirZ;
        vfloat16 minmax_org_maxmin_rdirX, minmax_org_maxmin_rdirY, minmax_org_maxmin_rdirZ;
#else
        size_t nearX, nearY, nearZ;
        size_t farX, farY, farZ;
#endif
        Vec3fa min_rdir; 
        Vec3fa max_rdir;
        
        Vec3fa max_org_min_rdir; 
        Vec3fa min_org_max_rdir; 

        float min_dist;
        float max_dist;
      };


    private:
      static void intersect1(const BVH* bvh, NodeRef root, const size_t k, Precalculations& pre, 
                             RayK<K>& ray, const Vec3vf<K> &ray_org, const Vec3vf<K> &ray_dir, const Vec3vf<K> &ray_rdir, const vfloat<K> &ray_tnear, const vfloat<K> &ray_tfar, const Vec3vi<K>& nearXYZ, IntersectContext* context);
      static bool occluded1(const BVH* bvh, NodeRef root, const size_t k, Precalculations& pre, 
                            RayK<K>& ray, const Vec3vf<K> &ray_org, const Vec3vf<K> &ray_dir, const Vec3vf<K> &ray_rdir, const vfloat<K> &ray_tnear, const vfloat<K> &ray_tfar, const Vec3vi<K>& nearXYZ, IntersectContext* context);

    public:
      static void intersect(vint<K>* valid, Accel::Intersectors* This, RayK<K>& ray, IntersectContext* context);
      static void occluded (vint<K>* valid, Accel::Intersectors* This, RayK<K>& ray, IntersectContext* context);

      static void intersect_coherent(vint<K>* valid, Accel::Intersectors* This, RayK<K>& ray, IntersectContext* context);
      static void occluded_coherent (vint<K>* valid, Accel::Intersectors* This, RayK<K>& ray, IntersectContext* context);

    };

    /*! BVH packet intersector. */
    template<int N, int K, int types, bool robust, typename PrimitiveIntersectorK>
      class BVHNIntersectorKChunk : public BVHNIntersectorKHybrid<N,K,types,robust,PrimitiveIntersectorK,false> {};
  }
}
