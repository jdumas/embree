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

#include "scene_subdiv_mesh.h"
#include "scene.h"
#include "../subdiv/patch_eval.h"
#include "../subdiv/patch_eval_simd.h"

#include "../algorithms/sort.h"
#include "../algorithms/prefix.h"
#include "../algorithms/parallel_for.h"

namespace embree
{
  SubdivMesh::SubdivMesh (Scene* parent, RTCGeometryFlags flags, size_t numFaces, size_t numEdges, size_t numVertices, 
			  size_t numEdgeCreases, size_t numVertexCreases, size_t numHoles, size_t numTimeSteps)
    : Geometry(parent,SUBDIV_MESH,numFaces,numTimeSteps,flags), 
      numFaces(numFaces), 
      numEdges(numEdges), 
      numVertices(numVertices),
      boundary(RTC_BOUNDARY_EDGE_ONLY),
      displFunc(nullptr),
      displFunc2(nullptr),
      displBounds(empty),
      tessellationRate(2.0f),
      numHalfEdges(0),
      faceStartEdge(parent->device),
      halfEdges(parent->device),
      invalid_face(parent->device),
      levelUpdate(false)
  {
    vertices.resize(numTimeSteps);
    vertex_buffer_tags.resize(numTimeSteps);
    for (size_t i=0; i<numTimeSteps; i++)
      vertices[i].init(parent->device,numVertices,sizeof(Vec3fa));

    vertexIndices.init(parent->device,numEdges,sizeof(unsigned int));
    faceVertices.init(parent->device,numFaces,sizeof(unsigned int));
    holes.init(parent->device,numHoles,sizeof(int));
    edge_creases.init(parent->device,numEdgeCreases,2*sizeof(unsigned int));
    edge_crease_weights.init(parent->device,numEdgeCreases,sizeof(float));
    vertex_creases.init(parent->device,numVertexCreases,sizeof(unsigned int));
    vertex_crease_weights.init(parent->device,numVertexCreases,sizeof(float));
    levels.init(parent->device,numEdges,sizeof(float));
    enabling();
  }

  void SubdivMesh::enabling() 
  { 
    parent->numSubdivEnableDisableEvents++;
    if (numTimeSteps == 1) parent->world1.numSubdivPatches += numFaces; 
    else                   parent->world2.numSubdivPatches += numFaces; 
  }
  
  void SubdivMesh::disabling() 
  { 
    parent->numSubdivEnableDisableEvents++;
    if (numTimeSteps == 1) parent->world1.numSubdivPatches -= numFaces; 
    else                   parent->world2.numSubdivPatches -= numFaces;
  }

  void SubdivMesh::setMask (unsigned mask) 
  {
    if (parent->isStatic() && parent->isBuild()) 
      throw_RTCError(RTC_INVALID_OPERATION,"static scenes cannot get modified");

    this->mask = mask; 
    Geometry::update();
  }

  void SubdivMesh::setBoundaryMode (RTCBoundaryMode mode)
  {
    if (parent->isStatic() && parent->isBuild()) 
      throw_RTCError(RTC_INVALID_OPERATION,"static scenes cannot get modified");

    if (boundary == mode) return;
    boundary = mode;
    updateBuffer(RTC_VERTEX_CREASE_WEIGHT_BUFFER);    
  }

  void SubdivMesh::setBuffer(RTCBufferType type, void* ptr, size_t offset, size_t stride) 
  { 
    if (parent->isStatic() && parent->isBuild()) 
      throw_RTCError(RTC_INVALID_OPERATION,"static scenes cannot get modified");

    /* verify that all accesses are 4 bytes aligned */
    if (((size_t(ptr) + offset) & 0x3) || (stride & 0x3)) 
      throw_RTCError(RTC_INVALID_OPERATION,"data must be 4 bytes aligned");

    if (type != RTC_LEVEL_BUFFER)
      parent->commitCounterSubdiv++;

    if (type >= RTC_VERTEX_BUFFER0 && type < RTCBufferType(RTC_VERTEX_BUFFER0 + numTimeSteps)) 
    {
      size_t t = type - RTC_VERTEX_BUFFER0;
      vertices[t].set(ptr,offset,stride); 
      vertices[t].checkPadding16();
    } 
    else 
    {
      switch (type) {
      case RTC_INDEX_BUFFER               : vertexIndices.set(ptr,offset,stride); break;
      case RTC_FACE_BUFFER                : faceVertices.set(ptr,offset,stride); break;
      case RTC_HOLE_BUFFER                : holes.set(ptr,offset,stride); break;
      case RTC_EDGE_CREASE_INDEX_BUFFER   : edge_creases.set(ptr,offset,stride); break;
      case RTC_EDGE_CREASE_WEIGHT_BUFFER  : edge_crease_weights.set(ptr,offset,stride); break;
      case RTC_VERTEX_CREASE_INDEX_BUFFER : vertex_creases.set(ptr,offset,stride); break;
      case RTC_VERTEX_CREASE_WEIGHT_BUFFER: vertex_crease_weights.set(ptr,offset,stride); break;
      case RTC_LEVEL_BUFFER               : levels.set(ptr,offset,stride); break;
        
      case RTC_USER_VERTEX_BUFFER0: 
        if (userbuffers[0] == nullptr) userbuffers[0].reset(new APIBuffer<char>(parent->device,numVertices,stride)); 
        userbuffers[0]->set(ptr,offset,stride);  
        userbuffers[0]->checkPadding16();
        break;
      case RTC_USER_VERTEX_BUFFER1: 
        if (userbuffers[1] == nullptr) userbuffers[1].reset(new APIBuffer<char>(parent->device,numVertices,stride)); 
        userbuffers[1]->set(ptr,offset,stride);  
        userbuffers[1]->checkPadding16();
        break;
        
      default: 
        throw_RTCError(RTC_INVALID_ARGUMENT,"unknown buffer type");
      }
    }
  }

  void* SubdivMesh::map(RTCBufferType type) 
  {
    if (parent->isStatic() && parent->isBuild())
      throw_RTCError(RTC_INVALID_OPERATION,"static scenes cannot get modified");

    if (type >= RTC_VERTEX_BUFFER0 && type < RTCBufferType(RTC_VERTEX_BUFFER0 + numTimeSteps)) {
      return vertices[type - RTC_VERTEX_BUFFER0].map(parent->numMappedBuffers);
    } 
    else
    {
      switch (type) {
      case RTC_INDEX_BUFFER                : return vertexIndices.map(parent->numMappedBuffers);
      case RTC_FACE_BUFFER                 : return faceVertices.map(parent->numMappedBuffers);
      case RTC_HOLE_BUFFER                 : return holes.map(parent->numMappedBuffers);
      case RTC_EDGE_CREASE_INDEX_BUFFER    : return edge_creases.map(parent->numMappedBuffers); 
      case RTC_EDGE_CREASE_WEIGHT_BUFFER   : return edge_crease_weights.map(parent->numMappedBuffers); 
      case RTC_VERTEX_CREASE_INDEX_BUFFER  : return vertex_creases.map(parent->numMappedBuffers); 
      case RTC_VERTEX_CREASE_WEIGHT_BUFFER : return vertex_crease_weights.map(parent->numMappedBuffers); 
      case RTC_LEVEL_BUFFER                : return levels.map(parent->numMappedBuffers); 
      default                              : throw_RTCError(RTC_INVALID_ARGUMENT,"unknown buffer type"); return nullptr;
      }
    }
  }

  void SubdivMesh::unmap(RTCBufferType type) 
  {
    if (parent->isStatic() && parent->isBuild())
      throw_RTCError(RTC_INVALID_OPERATION,"static scenes cannot get modified");

    if (type >= RTC_VERTEX_BUFFER0 && type < RTCBufferType(RTC_VERTEX_BUFFER0 + numTimeSteps)) {
      vertices[type - RTC_VERTEX_BUFFER0].unmap(parent->numMappedBuffers);
    }
    else
    {
      switch (type) {
      case RTC_INDEX_BUFFER               : vertexIndices.unmap(parent->numMappedBuffers); break;
      case RTC_FACE_BUFFER                : faceVertices.unmap(parent->numMappedBuffers); break;
      case RTC_HOLE_BUFFER                : holes.unmap(parent->numMappedBuffers); break;
      case RTC_EDGE_CREASE_INDEX_BUFFER   : edge_creases.unmap(parent->numMappedBuffers); break;
      case RTC_EDGE_CREASE_WEIGHT_BUFFER  : edge_crease_weights.unmap(parent->numMappedBuffers); break;
      case RTC_VERTEX_CREASE_INDEX_BUFFER : vertex_creases.unmap(parent->numMappedBuffers); break;
      case RTC_VERTEX_CREASE_WEIGHT_BUFFER: vertex_crease_weights.unmap(parent->numMappedBuffers); break;
      case RTC_LEVEL_BUFFER               : levels.unmap(parent->numMappedBuffers); break;
      default                             : throw_RTCError(RTC_INVALID_ARGUMENT,"unknown buffer type"); break;
      }
    }
  }

  void SubdivMesh::update ()
  {
    faceVertices.setModified(true);
    vertexIndices.setModified(true); 
    holes.setModified(true);
    for (auto& buffer : vertices) buffer.setModified(true); 
    edge_creases.setModified(true);
    edge_crease_weights.setModified(true);
    vertex_creases.setModified(true);
    vertex_crease_weights.setModified(true); 
    levels.setModified(true);
    Geometry::update();
  }

  void SubdivMesh::updateBuffer (RTCBufferType type)
  {
    if (type != RTC_LEVEL_BUFFER)
      parent->commitCounterSubdiv++;

    if (type >= RTC_VERTEX_BUFFER0 && type < RTCBufferType(RTC_VERTEX_BUFFER0 + numTimeSteps)) {
      vertices[type - RTC_VERTEX_BUFFER0].setModified(true);
    }
    else
    {
      switch (type) {
      case RTC_INDEX_BUFFER               : vertexIndices.setModified(true); break;
      case RTC_FACE_BUFFER                : faceVertices.setModified(true); break;
      case RTC_HOLE_BUFFER                : holes.setModified(true); break;
      case RTC_EDGE_CREASE_INDEX_BUFFER   : edge_creases.setModified(true); break;
      case RTC_EDGE_CREASE_WEIGHT_BUFFER  : edge_crease_weights.setModified(true); break;
      case RTC_VERTEX_CREASE_INDEX_BUFFER : vertex_creases.setModified(true); break;
      case RTC_VERTEX_CREASE_WEIGHT_BUFFER: vertex_crease_weights.setModified(true); break;
      case RTC_LEVEL_BUFFER               : levels.setModified(true); break;
      default                             : throw_RTCError(RTC_INVALID_ARGUMENT,"unknown buffer type"); break;
      }
    }
    Geometry::update();
  }

  void SubdivMesh::setDisplacementFunction (RTCDisplacementFunc func, RTCBounds* bounds) 
  {
    if (parent->isStatic() && parent->isBuild())
      throw_RTCError(RTC_INVALID_OPERATION,"static scenes cannot get modified");

    this->displFunc   = func;
    if (bounds) this->displBounds = *(BBox3fa*)bounds; 
    else        this->displBounds = empty;
  }

  void SubdivMesh::setDisplacementFunction2 (RTCDisplacementFunc2 func, RTCBounds* bounds) 
  {
    if (parent->isStatic() && parent->isBuild())
      throw_RTCError(RTC_INVALID_OPERATION,"static scenes cannot get modified");

    this->displFunc2   = func;
    if (bounds) this->displBounds = *(BBox3fa*)bounds; 
    else        this->displBounds = empty;
  }

  void SubdivMesh::setTessellationRate(float N)
  {
    if (parent->isStatic() && parent->isBuild()) 
      throw_RTCError(RTC_INVALID_OPERATION,"static geometries cannot get modified");

    tessellationRate = N;
    levels.setModified(true);
  }

  void SubdivMesh::immutable () 
  {
    const bool freeIndices = !parent->needSubdivIndices;
    const bool freeVertices = !parent->needSubdivVertices;
    faceVertices.free();
    if (freeIndices) vertexIndices.free();
    if (freeVertices )
      for (auto& buffer : vertices)
        buffer.free();
    edge_creases.free();
    edge_crease_weights.free();
    vertex_creases.free();
    vertex_crease_weights.free();
    levels.free();
    holes.free();
  }

  __forceinline uint64_t pair64(unsigned int x, unsigned int y) 
  {
    if (x<y) std::swap(x,y);
    return (((uint64_t)x) << 32) | (uint64_t)y;
  }

  void SubdivMesh::calculateHalfEdges()
  {
    size_t blockSize = 4096;

    /* allocate temporary array */
    invalid_face.resize(numFaces*numTimeSteps);
    halfEdges0.resize(numEdges);
    halfEdges1.resize(numEdges);

    /* create all half edges */
    parallel_for( size_t(0), numFaces, blockSize, [&](const range<size_t>& r) 
    {
      for (size_t f=r.begin(); f<r.end(); f++) 
      {
	const unsigned N = faceVertices[f];
	const unsigned e = faceStartEdge[f];

	for (unsigned de=0; de<N; de++)
	{
	  HalfEdge* edge = &halfEdges[e+de];

	  const unsigned int startVertex = vertexIndices[e+de];
	  unsigned int nextIndex = de + 1;
	  if (unlikely(nextIndex >= N)) nextIndex -= N; 
	  const unsigned int endVertex = vertexIndices[e + nextIndex]; 
	  const uint64_t key = SubdivMesh::Edge(startVertex,endVertex);
	  
	  edge->vtx_index              = startVertex;
	  edge->next_half_edge_ofs     = (de == (N-1)) ? -int(N-1) : +1;
	  edge->prev_half_edge_ofs     = (de ==     0) ? +int(N-1) : -1;
	  edge->opposite_half_edge_ofs = 0;
	  edge->edge_crease_weight     = edgeCreaseMap.lookup(key,0.0f);
	  edge->vertex_crease_weight   = vertexCreaseMap.lookup(startVertex,0.0f);
	  edge->edge_level             = getEdgeLevel(e+de);
          edge->patch_type             = HalfEdge::COMPLEX_PATCH; // type gets updated below
          edge->vertex_type            = HalfEdge::REGULAR_VERTEX;

	  if (unlikely(holeSet.lookup(unsigned(f)))) 
	    halfEdges1[e+de] = SubdivMesh::KeyHalfEdge(std::numeric_limits<uint64_t>::max(),edge);
	  else
	    halfEdges1[e+de] = SubdivMesh::KeyHalfEdge(key,edge);
	}
      }
    });

    /* sort half edges to find adjacent edges */
    radix_sort_u64(halfEdges1.data(),halfEdges0.data(),numHalfEdges);

    /* link all adjacent pairs of edges */
    parallel_for( size_t(0), numHalfEdges, blockSize, [&](const range<size_t>& r) 
    {
      /* skip if start of adjacent edges was not in our range */
      size_t e=r.begin();
      if (e != 0 && (halfEdges1[e].key == halfEdges1[e-1].key)) {
	const uint64_t key = halfEdges1[e].key;
	while (e<r.end() && halfEdges1[e].key == key) e++;
      }

      /* process all adjacent edges starting in our range */
      while (e<r.end())
      {
	const uint64_t key = halfEdges1[e].key;
	if (key == std::numeric_limits<uint64_t>::max()) break;
	size_t N=1; while (e+N<numHalfEdges && halfEdges1[e+N].key == key) N++;

        /* border edges are identified by not having an opposite edge set */
	if (N == 1) {
          halfEdges1[e].edge->edge_crease_weight = float(inf);
	}

        /* standard edge shared between two faces */
        else if (N == 2)
        {
          if (halfEdges1[e+0].edge->next()->vtx_index != halfEdges1[e+1].edge->vtx_index) // FIXME: workaround for wrong winding order of opposite patch
          {
            halfEdges1[e+0].edge->edge_crease_weight = float(inf);
            halfEdges1[e+1].edge->edge_crease_weight = float(inf);
          }
          else {
            halfEdges1[e+0].edge->setOpposite(halfEdges1[e+1].edge);
            halfEdges1[e+1].edge->setOpposite(halfEdges1[e+0].edge);
          }
	}

        /* non-manifold geometry is handled by keeping vertices fixed during subdivision */
        else {
	  for (size_t i=0; i<N; i++) {
	    halfEdges1[e+i].edge->vertex_crease_weight = inf;
            halfEdges1[e+i].edge->vertex_type = HalfEdge::NON_MANIFOLD_EDGE_VERTEX;
            halfEdges1[e+i].edge->edge_crease_weight = inf;

	    halfEdges1[e+i].edge->next()->vertex_crease_weight = inf;
            halfEdges1[e+i].edge->next()->vertex_type = HalfEdge::NON_MANIFOLD_EDGE_VERTEX;
            halfEdges1[e+i].edge->next()->edge_crease_weight = inf;
	  }
	}
	e+=N;
      }
    });

    /* calculate patch types and sharp corners */
    parallel_for( size_t(0), numFaces, blockSize, [&](const range<size_t>& r) 
    {
      for (size_t f=r.begin(); f<r.end(); f++) 
      {
        HalfEdge* edge = &halfEdges[faceStartEdge[f]];
        HalfEdge::PatchType patch_type = edge->patchType();
        for (size_t t=0; t<numTimeSteps; t++)
          invalidFace(f,t) = !edge->valid(vertices[t]) || holeSet.lookup(unsigned(f));
          
        for (size_t i=0; i<faceVertices[f]; i++) 
        {
          edge[i].patch_type = patch_type;

          /* calculate sharp corner vertices */
          if (boundary == RTC_BOUNDARY_EDGE_AND_CORNER && edge[i].isCorner()) 
            edge[i].vertex_crease_weight = float(inf);
        }
      }
    });
  }

  void SubdivMesh::updateHalfEdges()
  {
    /* assume we do no longer recalculate in the future and clear these arrays */
    halfEdges0.clear();
    halfEdges1.clear();

    /* calculate which data to update */
    const bool updateEdgeCreases = edge_creases.isModified() || edge_crease_weights.isModified();
    const bool updateVertexCreases = vertex_creases.isModified() || vertex_crease_weights.isModified(); 
    const bool updateLevels = levels.isModified();

    /* parallel loop over all half edges */
    parallel_for( size_t(0), numHalfEdges, size_t(4096), [&](const range<size_t>& r) 
    {
      for (size_t i=r.begin(); i!=r.end(); i++)
      {
	HalfEdge& edge = halfEdges[i];
	const unsigned int startVertex = edge.vtx_index;
 
	if (updateLevels)
	  edge.edge_level = getEdgeLevel(i); 
        
	if (updateEdgeCreases) {
	  const unsigned int endVertex   = edge.next()->vtx_index;
	  const uint64_t key = SubdivMesh::Edge(startVertex,endVertex);
	  if (edge.hasOpposite()) // leave weight at inf for borders
            edge.edge_crease_weight = edgeCreaseMap.lookup(key,0.0f);
	}

        if (updateVertexCreases && edge.vertex_type != HalfEdge::NON_MANIFOLD_EDGE_VERTEX) {
	  edge.vertex_crease_weight = vertexCreaseMap.lookup(startVertex,0.0f);
          if (boundary == RTC_BOUNDARY_EDGE_AND_CORNER && edge.isCorner()) 
            edge.vertex_crease_weight = float(inf);
        }

        if (updateEdgeCreases || updateVertexCreases) {
          edge.patch_type = edge.patchType();
        }
      }
    });
  }

  void SubdivMesh::initializeHalfEdgeStructures ()
  {
    double t0 = getSeconds();

    /* allocate half edge array */
    halfEdges.resize(numEdges);
    
    /* calculate start edge of each face */
    faceStartEdge.resize(numFaces);
    if (faceVertices.isModified()) 
      numHalfEdges = parallel_prefix_sum(faceVertices,faceStartEdge,numFaces,std::plus<int>());

    /* create set with all holes */
    if (holes.isModified())
      holeSet.init(holes);

    /* create set with all vertex creases */
    if (vertex_creases.isModified() || vertex_crease_weights.isModified())
      vertexCreaseMap.init(vertex_creases,vertex_crease_weights);
    
    /* create map with all edge creases */
    if (edge_creases.isModified() || edge_crease_weights.isModified())
      edgeCreaseMap.init(edge_creases,edge_crease_weights);

    /* check if we have to recalculate the half edges */
    bool recalculate = false;
    recalculate |= vertexIndices.isModified(); 
    recalculate |= faceVertices.isModified();
    recalculate |= holes.isModified();

    /* check if we can simply update the half edges */
    bool update = false;
    update |= edge_creases.isModified();
    update |= edge_crease_weights.isModified();
    update |= vertex_creases.isModified();
    update |= vertex_crease_weights.isModified(); 
    update |= levels.isModified();
    
    /* check whether we can simply update the bvh in cached mode */
    levelUpdate = !recalculate && edge_creases.size() == 0 && vertex_creases.size() == 0 && levels.isModified();

    /* now either recalculate or update the half edges */
    if (recalculate) calculateHalfEdges();
    else if (update) updateHalfEdges();

    /* create interpolation cache mapping for interpolatable meshes */
    if (parent->isInterpolatable()) 
    {
#if defined (__TARGET_AVX__)
      auto numInterpolationSlots = [&] (size_t stride) {
        if (parent->device->hasISA(AVX)) return numInterpolationSlots8(stride); else return numInterpolationSlots4(stride);
      };
#else
      auto numInterpolationSlots = [] (size_t stride) {
        return numInterpolationSlots4(stride);
      };
#endif
      for (size_t i=0; i<numTimeSteps; i++)
        vertex_buffer_tags[i].resize(numFaces*numInterpolationSlots(vertices[i].getStride()));
      for (size_t i=0; i<2; i++)
        if (userbuffers[i]) user_buffer_tags  [i].resize(numFaces*numInterpolationSlots(userbuffers[i]->getStride()));
    }

    /* cleanup some state for static scenes */
    if (parent->isStatic()) 
    {
      holeSet.cleanup();
      halfEdges0.clear();
      halfEdges1.clear();
      vertexCreaseMap.clear();
      edgeCreaseMap.clear();
    }

    /* clear modified state of all buffers */
    vertexIndices.setModified(false); 
    faceVertices.setModified(false);
    holes.setModified(false);
    for (auto& buffer : vertices) buffer.setModified(false); 
    edge_creases.setModified(false);
    edge_crease_weights.setModified(false);
    vertex_creases.setModified(false);
    vertex_crease_weights.setModified(false); 
    levels.setModified(false);

    double t1 = getSeconds();

    /* print statistics in verbose mode */
    if (parent->device->verbosity(2)) 
    {
      size_t numRegularQuadFaces = 0;
      size_t numIrregularQuadFaces = 0;
      size_t numComplexFaces = 0;

      for (size_t e=0, f=0; f<numFaces; e+=faceVertices[f++]) 
      {
        switch (halfEdges[e].patch_type) {
        case HalfEdge::REGULAR_QUAD_PATCH  : numRegularQuadFaces++;   break;
        case HalfEdge::IRREGULAR_QUAD_PATCH: numIrregularQuadFaces++; break;
        case HalfEdge::COMPLEX_PATCH       : numComplexFaces++;   break;
        }
      }
    
      std::cout << "half edge generation = " << 1000.0*(t1-t0) << "ms, " << 1E-6*double(numHalfEdges)/(t1-t0) << "M/s" << std::endl;
      std::cout << "numFaces = " << numFaces << ", " 
                << "numRegularQuadFaces = " << numRegularQuadFaces << " (" << 100.0f * numRegularQuadFaces / numFaces << "%), " 
                << "numIrregularQuadFaces " << numIrregularQuadFaces << " (" << 100.0f * numIrregularQuadFaces / numFaces << "%) " 
                << "numComplexFaces " << numComplexFaces << " (" << 100.0f * numComplexFaces / numFaces << "%) " 
                << std::endl;
    }
  }

  bool SubdivMesh::verify () 
  {
    /*! verify consistent size of vertex arrays */
    if (vertices.size() == 0) return false;
    for (const auto& buffer : vertices)
      if (vertices[0].size() != buffer.size())
        return false;

    /*! verify vertex indices */
    size_t ofs = 0;
    for (size_t i=0; i<faceVertices.size(); i++) 
    {
      int valence = faceVertices[i];
      for (size_t j=ofs; j<ofs+valence; j++) 
      {
        if (j >= vertexIndices.size())
          return false;

        if (vertexIndices[j] >= numVertices)
          return false; 
      }
      ofs += valence;
    }

    /*! verify vertices */
    for (const auto& buffer : vertices)
      for (size_t i=0; i<buffer.size(); i++)
	if (!isvalid(buffer[i])) 
	  return false;

    return true;
  }

  void SubdivMesh::interpolate(unsigned primID, float u, float v, RTCBufferType buffer, float* P, float* dPdu, float* dPdv, float* ddPdudu, float* ddPdvdv, float* ddPdudv, size_t numFloats) 
  {
    /* test if interpolation is enabled */
#if defined(DEBUG) 
    if ((parent->aflags & RTC_INTERPOLATE) == 0) 
      throw_RTCError(RTC_INVALID_OPERATION,"rtcInterpolate can only get called when RTC_INTERPOLATE is enabled for the scene");
#endif

    /* calculate base pointer and stride */
    assert((buffer >= RTC_VERTEX_BUFFER0 && buffer < RTCBufferType(RTC_VERTEX_BUFFER0 + numTimeSteps)) ||
           (buffer >= RTC_USER_VERTEX_BUFFER0 && buffer <= RTC_USER_VERTEX_BUFFER1));
    const char* src = nullptr; 
    size_t stride = 0;
    size_t bufID = buffer&0xFFFF;
    std::vector<SharedLazyTessellationCache::CacheEntry>* baseEntry = nullptr;
    if (buffer >= RTC_USER_VERTEX_BUFFER0) {
      src    = userbuffers[bufID]->getPtr();
      stride = userbuffers[bufID]->getStride();
      baseEntry = &user_buffer_tags[bufID];
    } else {
      src    = vertices[bufID].getPtr();
      stride = vertices[bufID].getStride();
      baseEntry = &vertex_buffer_tags[bufID];
    }

    for (size_t i=0; i<numFloats; i+=4)
    {
      vfloat4 Pt, dPdut, dPdvt, ddPdudut, ddPdvdvt, ddPdudvt;
      isa::PatchEval<vfloat4,vfloat4>(baseEntry->at(interpolationSlot(primID,i/4,stride)),parent->commitCounterSubdiv,
                                      getHalfEdge(primID),src+i*sizeof(float),stride,u,v,
                                      P ? &Pt : nullptr, 
                                      dPdu ? &dPdut : nullptr, 
                                      dPdv ? &dPdvt : nullptr,
                                      ddPdudu ? &ddPdudut : nullptr, 
                                      ddPdvdv ? &ddPdvdvt : nullptr, 
                                      ddPdudv ? &ddPdudvt : nullptr);

      if (P) {
        for (size_t j=i; j<min(i+4,numFloats); j++) 
          P[j] = Pt[j-i];
      }
      if (dPdu) 
      {
        for (size_t j=i; j<min(i+4,numFloats); j++) {
          dPdu[j] = dPdut[j-i];
          dPdv[j] = dPdvt[j-i];
        }
      }
      if (ddPdudu) 
      {
        for (size_t j=i; j<min(i+4,numFloats); j++) {
          ddPdudu[j] = ddPdudut[j-i];
          ddPdvdv[j] = ddPdvdvt[j-i];
          ddPdudv[j] = ddPdudvt[j-i];
        }
      }
    }
  }

  void SubdivMesh::interpolateN(const void* valid_i, const unsigned* primIDs, const float* u, const float* v, size_t numUVs, 
                                RTCBufferType buffer, float* P, float* dPdu, float* dPdv, float* ddPdudu, float* ddPdvdv, float* ddPdudv, size_t numFloats)
  {
    /* test if interpolation is enabled */
#if defined(DEBUG)
    if ((parent->aflags & RTC_INTERPOLATE) == 0) 
      throw_RTCError(RTC_INVALID_OPERATION,"rtcInterpolate can only get called when RTC_INTERPOLATE is enabled for the scene");
#endif

    /* calculate base pointer and stride */
    assert((buffer >= RTC_VERTEX_BUFFER0 && buffer < RTCBufferType(RTC_VERTEX_BUFFER0 + numTimeSteps)) ||
           (buffer >= RTC_USER_VERTEX_BUFFER0 && buffer <= RTC_USER_VERTEX_BUFFER1));
    const char* src = nullptr; 
    size_t stride = 0;
    size_t bufID = buffer&0xFFFF;
    std::vector<SharedLazyTessellationCache::CacheEntry>* baseEntry = nullptr;
    if (buffer >= RTC_USER_VERTEX_BUFFER0) {
      src    = userbuffers[bufID]->getPtr();
      stride = userbuffers[bufID]->getStride();
      baseEntry = &user_buffer_tags[bufID];
    } else {
      src    = vertices[bufID].getPtr();
      stride = vertices[bufID].getStride();
      baseEntry = &vertex_buffer_tags[bufID];
    }

    const int* valid = (const int*) valid_i;
    
    for (size_t i=0; i<numUVs; i+=4) 
    {
      vbool4 valid1 = vint4(i)+vint4(step) < vint4(numUVs);
      if (valid) valid1 &= vint4::loadu(&valid[i]) == vint4(-1);
      if (none(valid1)) continue;
      
      const vint4 primID = vint4::loadu(&primIDs[i]);
      const vfloat4 uu = vfloat4::loadu(&u[i]);
      const vfloat4 vv = vfloat4::loadu(&v[i]);

      foreach_unique(valid1,primID,[&](const vbool4& valid1, const int primID)
      {
        for (size_t j=0; j<numFloats; j+=4) 
        {
          const size_t M = min(size_t(4),numFloats-j);
          isa::PatchEvalSimd<vbool4,vint4,vfloat4,vfloat4>(baseEntry->at(interpolationSlot(primID,j/4,stride)),parent->commitCounterSubdiv,
                                                           getHalfEdge(primID),src+j*sizeof(float),stride,valid1,uu,vv,
                                                           P ? P+j*numUVs+i : nullptr,
                                                           dPdu ? dPdu+j*numUVs+i : nullptr,
                                                           dPdv ? dPdv+j*numUVs+i : nullptr,
                                                           ddPdudu ? ddPdudu+j*numUVs+i : nullptr,
                                                           ddPdvdv ? ddPdvdv+j*numUVs+i : nullptr,
                                                           ddPdudv ? ddPdudv+j*numUVs+i : nullptr,
                                                           numUVs,M);
        }
      });
    }
  }
}
