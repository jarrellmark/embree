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

#include "../common/math/random_sampler.h"
#include "../common/math/sampling.h"
#include "../common/tutorial/tutorial_device.h"
#include "../common/tutorial/scene_device.h"

namespace embree {

#define USE_INTERFACE 0 // 0 = stream, 1 = single rays/packets, 2 = single rays/packets using stream interface
#define AMBIENT_OCCLUSION_SAMPLES 64
//#define rtcOccluded1 rtcIntersect1
//#define rtcOccluded1M rtcIntersect1M

#define SIMPLE_SHADING 1

extern "C" ISPCScene* g_ispc_scene;
extern "C" int g_instancing_mode;

/* scene data */
RTCDevice g_device = nullptr;
RTCScene g_scene = nullptr;

#define MAX_EDGE_LEVEL 64.0f
#define MIN_EDGE_LEVEL  4.0f
#define LEVEL_FACTOR   64.0f

  inline float updateEdgeLevel( ISPCSubdivMesh* mesh, const Vec3fa& cam_pos, const size_t e0, const size_t e1)
{
  const Vec3fa v0 = mesh->positions[0][mesh->position_indices[e0]];
  const Vec3fa v1 = mesh->positions[0][mesh->position_indices[e1]];
  const Vec3fa edge = v1-v0;
  const Vec3fa P = 0.5f*(v1+v0);
  const Vec3fa dist = cam_pos - P;
  return max(min(LEVEL_FACTOR*(0.5f*length(edge)/length(dist)),MAX_EDGE_LEVEL),MIN_EDGE_LEVEL);
}


void updateEdgeLevelBuffer( ISPCSubdivMesh* mesh, const Vec3fa& cam_pos, size_t startID, size_t endID )
{
  for (size_t f=startID; f<endID;f++) {
    unsigned int e = mesh->face_offsets[f];
    unsigned int N = mesh->verticesPerFace[f];
    if (N == 4) /* fast path for quads */
      for (size_t i=0; i<4; i++)
        mesh->subdivlevel[e+i] =  updateEdgeLevel(mesh,cam_pos,e+(i+0),e+(i+1)%4);
    else if (N == 3) /* fast path for triangles */
      for (size_t i=0; i<3; i++)
        mesh->subdivlevel[e+i] =  updateEdgeLevel(mesh,cam_pos,e+(i+0),e+(i+1)%3);
    else /* fast path for general polygons */
      for (size_t i=0; i<N; i++)
        mesh->subdivlevel[e+i] =  updateEdgeLevel(mesh,cam_pos,e+(i+0),e+(i+1)%N);
  }
}

#if defined(ISPC)
void updateSubMeshEdgeLevelBufferTask (int taskIndex, int threadIndex,  ISPCSubdivMesh* mesh, const Vec3fa& cam_pos )
{
  const size_t size = mesh->numFaces;
  const size_t startID = ((taskIndex+0)*size)/taskCount;
  const size_t endID   = ((taskIndex+1)*size)/taskCount;
  updateEdgeLevelBuffer(mesh,cam_pos,startID,endID);
}
void updateMeshEdgeLevelBufferTask (int taskIndex, int threadIndex,  ISPCScene* scene_in, const Vec3fa& cam_pos )
{
  ISPCGeometry* geometry = g_ispc_scene->geometries[taskIndex];
  if (geometry->type != SUBDIV_MESH) return;
  ISPCSubdivMesh* mesh = (ISPCSubdivMesh*) geometry;
  unsigned int geomID = mesh->geom.geomID;
  if (mesh->numFaces < 10000) {
    updateEdgeLevelBuffer(mesh,cam_pos,0,mesh->numFaces);
    rtcUpdateBuffer(rtcGetGeometry(g_scene,mesh->geom.geomID),RTC_LEVEL_BUFFER);
  }
}
#endif

void updateEdgeLevels(ISPCScene* scene_in, const Vec3fa& cam_pos)
{
  /* first update small meshes */
#if defined(ISPC)
  parallel_for(size_t(0),size_t( scene_in->numGeometries ),[&](const range<size_t>& range) {
    const int threadIndex = (int)TaskScheduler::threadIndex();
    for (size_t i=range.begin(); i<range.end(); i++)
      updateMeshEdgeLevelBufferTask((int)i,threadIndex,scene_in,cam_pos);
  }); 
#endif

  /* now update large meshes */
  for (size_t g=0; g<scene_in->numGeometries; g++)
  {
    ISPCGeometry* geometry = g_ispc_scene->geometries[g];
    if (geometry->type != SUBDIV_MESH) continue;
    ISPCSubdivMesh* mesh = (ISPCSubdivMesh*) geometry;
#if defined(ISPC)
    if (mesh->numFaces < 10000) continue;
    parallel_for(size_t(0),size_t( (mesh->numFaces+4095)/4096 ),[&](const range<size_t>& range) {
    const int threadIndex = (int)TaskScheduler::threadIndex();
    for (size_t i=range.begin(); i<range.end(); i++)
      updateSubMeshEdgeLevelBufferTask((int)i,threadIndex,mesh,cam_pos);
  }); 
#else
    updateEdgeLevelBuffer(mesh,cam_pos,0,mesh->numFaces);
#endif
    rtcUpdateBuffer(rtcGetGeometry(g_scene,mesh->geom.geomID),RTC_LEVEL_BUFFER);
  }
}
  
RTCScene convertScene(ISPCScene* scene_in)
{
  int scene_flags = RTC_SCENE_STATIC | RTC_SCENE_INCOHERENT;
  int scene_aflags = RTC_INTERSECT1 | RTC_INTERSECT_STREAM | RTC_INTERPOLATE;
  RTCScene scene_out = ConvertScene(g_device, scene_in,(RTCSceneFlags)scene_flags, (RTCAlgorithmFlags) scene_aflags, RTC_GEOMETRY_STATIC);

  /* commit individual objects in case of instancing */
  if (g_instancing_mode == ISPC_INSTANCING_SCENE_GEOMETRY || g_instancing_mode == ISPC_INSTANCING_SCENE_GROUP)
  {
    for (unsigned int i=0; i<scene_in->numGeometries; i++) {
      if (scene_in->geomID_to_scene[i]) rtcCommit(scene_in->geomID_to_scene[i]);
    }
  }
  return scene_out;
}

/* renders a single pixel casting with ambient occlusion */
Vec3fa ambientOcclusionShading(int x, int y, RTCRay& ray, RayStats& stats)
{
  RTCRay rays[AMBIENT_OCCLUSION_SAMPLES];

  Vec3fa Ng = normalize(ray.Ng);
  if (dot(ray.dir,Ng) > 0.0f) Ng = neg(Ng);

  Vec3fa col = Vec3fa(min(1.0f,0.3f+0.8f*abs(dot(Ng,normalize(ray.dir)))));

  /* calculate hit point */
  float intensity = 0;
  Vec3fa hitPos = ray.org + ray.tfar * ray.dir;

  RandomSampler sampler;
  RandomSampler_init(sampler,x,y,0);

  /* enable only valid rays */
  for (int i=0; i<AMBIENT_OCCLUSION_SAMPLES; i++)
  {
    /* sample random direction */
    Vec2f s = RandomSampler_get2D(sampler);
    Sample3f dir;
    dir.v = cosineSampleHemisphere(s);
    dir.pdf = cosineSampleHemispherePDF(dir.v);
    dir.v = frame(Ng) * dir.v;

    /* initialize shadow ray */
    RTCRay& shadow = rays[i];
    shadow.org = hitPos;
    shadow.dir = dir.v;
    bool mask = 1; { // invalidate inactive rays
      shadow.tnear = mask ? 0.001f       : (float)(pos_inf);
      shadow.tfar  = mask ? (float)(inf) : (float)(neg_inf);
    }
    shadow.geomID = RTC_INVALID_GEOMETRY_ID;
    shadow.primID = RTC_INVALID_GEOMETRY_ID;
    shadow.mask = -1;
    shadow.time = 0;
    RayStats_addShadowRay(stats);
  }

  RTCIntersectContext context;
  context.flags = g_iflags_incoherent;

  /* trace occlusion rays */
#if USE_INTERFACE == 0
  rtcOccluded1M(g_scene,&context,rays,AMBIENT_OCCLUSION_SAMPLES,sizeof(RTCRay));
#elif USE_INTERFACE == 1
  for (size_t i=0; i<AMBIENT_OCCLUSION_SAMPLES; i++)
    rtcOccluded1(g_scene,rays[i]);
#else
  for (size_t i=0; i<AMBIENT_OCCLUSION_SAMPLES; i++)
    rtcOccluded1M(g_scene,&context,&rays[i],1,sizeof(RTCRay));
#endif

  /* accumulate illumination */
  for (int i=0; i<AMBIENT_OCCLUSION_SAMPLES; i++) {
    if (rays[i].geomID == RTC_INVALID_GEOMETRY_ID)
      intensity += 1.0f;
  }

  /* shade pixel */
  return col * (intensity/AMBIENT_OCCLUSION_SAMPLES);
}


void postIntersectGeometry(const RTCRay& ray, DifferentialGeometry& dg, ISPCGeometry* geometry, int& materialID)
{
  if (geometry->type == TRIANGLE_MESH)
  {
    ISPCTriangleMesh* mesh = (ISPCTriangleMesh*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == QUAD_MESH)
  {
    ISPCQuadMesh* mesh = (ISPCQuadMesh*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == SUBDIV_MESH)
  {
    ISPCSubdivMesh* mesh = (ISPCSubdivMesh*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == LINE_SEGMENTS)
  {
    ISPCLineSegments* mesh = (ISPCLineSegments*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == HAIR_SET)
  {
    ISPCHairSet* mesh = (ISPCHairSet*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == CURVES)
  {
    ISPCHairSet* mesh = (ISPCHairSet*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == GROUP) {
    unsigned int geomID = ray.geomID; {
      postIntersectGeometry(ray,dg,((ISPCGroup*) geometry)->geometries[geomID],materialID);
    }
  }
  else
    assert(false);
}

AffineSpace3fa calculate_interpolated_space (ISPCInstance* instance, float gtime)
{
  if (instance->numTimeSteps == 1)
    return AffineSpace3fa(instance->spaces[0]);

   /* calculate time segment itime and fractional time ftime */
  const int time_segments = instance->numTimeSteps-1;
  const float time = gtime*(float)(time_segments);
  const int itime = clamp((int)(floor(time)),(int)0,time_segments-1);
  const float ftime = time - (float)(itime);
  return (1.0f-ftime)*AffineSpace3fa(instance->spaces[itime+0]) + ftime*AffineSpace3fa(instance->spaces[itime+1]);
}

inline int postIntersect(const RTCRay& ray, DifferentialGeometry& dg)
{
  int materialID = 0;
  unsigned int instID = ray.instID; {
    unsigned int geomID = ray.geomID; {
      ISPCGeometry* geometry = nullptr;
      if (g_instancing_mode == ISPC_INSTANCING_SCENE_GEOMETRY || g_instancing_mode == ISPC_INSTANCING_SCENE_GROUP) {
        ISPCInstance* instance = g_ispc_scene->geomID_to_inst[instID];
        geometry = g_ispc_scene->geometries[instance->geom.geomID];
      } else {
        geometry = g_ispc_scene->geometries[geomID];
      }
      postIntersectGeometry(ray,dg,geometry,materialID);
    }
  }

  if (g_instancing_mode != ISPC_INSTANCING_NONE)
  {
    unsigned int instID = ray.instID;
    {
      /* get instance and geometry pointers */
      ISPCInstance* instance = g_ispc_scene->geomID_to_inst[instID];

      /* convert normals */
      //AffineSpace3fa space = (1.0f-ray.time)*AffineSpace3fa(instance->space0) + ray.time*AffineSpace3fa(instance->space1);
      AffineSpace3fa space = calculate_interpolated_space(instance,ray.time);
      dg.Ng = xfmVector(space,dg.Ng);
      dg.Ns = xfmVector(space,dg.Ns);
    }
  }

  return materialID;
}

inline Vec3fa face_forward(const Vec3fa& dir, const Vec3fa& _Ng) {
  const Vec3fa Ng = _Ng;
  return dot(dir,Ng) < 0.0f ? Ng : neg(Ng);
}

/* renders a single screen tile */
void renderTileStandard(int taskIndex,
                        int threadIndex,
                        int* pixels,
                        const unsigned int width,
                        const unsigned int height,
                        const float time,
                        const ISPCCamera& camera,
                        const int numTilesX,
                        const int numTilesY)
{
  const unsigned int tileY = taskIndex / numTilesX;
  const unsigned int tileX = taskIndex - tileY * numTilesX;
  const unsigned int x0 = tileX * TILE_SIZE_X;
  const unsigned int x1 = min(x0+TILE_SIZE_X,width);
  const unsigned int y0 = tileY * TILE_SIZE_Y;
  const unsigned int y1 = min(y0+TILE_SIZE_Y,height);

  RayStats& stats = g_stats[threadIndex];

  RTCRay rays[TILE_SIZE_X*TILE_SIZE_Y];

  /* generate stream of primary rays */
  int N = 0;
  for (unsigned int y=y0; y<y1; y++) for (unsigned int x=x0; x<x1; x++)
  {
    /* ISPC workaround for mask == 0 */
    

    RandomSampler sampler;
    RandomSampler_init(sampler, x, y, 0);

    /* initialize ray */
    RTCRay& ray = rays[N++];
    ray.org = Vec3fa(camera.xfm.p);
    ray.dir = Vec3fa(normalize((float)x*camera.xfm.l.vx + (float)y*camera.xfm.l.vy + camera.xfm.l.vz));
    bool mask = 1; { // invalidates inactive rays
      ray.tnear = mask ? 0.0f         : (float)(pos_inf);
      ray.tfar  = mask ? (float)(inf) : (float)(neg_inf);
    }
    ray.geomID = RTC_INVALID_GEOMETRY_ID;
    ray.primID = RTC_INVALID_GEOMETRY_ID;
    ray.mask = -1;
    ray.time = RandomSampler_get1D(sampler);
    RayStats_addRay(stats);
  }

  RTCIntersectContext context;
  context.flags = g_iflags_coherent;

  /* trace stream of rays */
#if USE_INTERFACE == 0
  rtcIntersect1M(g_scene,&context,rays,N,sizeof(RTCRay));
#elif USE_INTERFACE == 1
  for (size_t i=0; i<N; i++)
    rtcIntersect1(g_scene,&context,rays[i]);
#else
  for (size_t i=0; i<N; i++)
    rtcIntersect1M(g_scene,&context,&rays[i],1,sizeof(RTCRay));
#endif

  /* shade stream of rays */
  N = 0;
  for (unsigned int y=y0; y<y1; y++) for (unsigned int x=x0; x<x1; x++)
  {
    /* ISPC workaround for mask == 0 */
    
    RTCRay& ray = rays[N++];

    /* eyelight shading */
    Vec3fa color = Vec3fa(0.0f);
    if (ray.geomID != RTC_INVALID_GEOMETRY_ID)
#if SIMPLE_SHADING == 1
    {
      Vec3fa Kd = Vec3fa(0.5f);
      DifferentialGeometry dg;
      dg.geomID = ray.geomID;
      dg.primID = ray.primID;
      dg.u = ray.u;
      dg.v = ray.v;
      dg.P  = ray.org+ray.tfar*ray.dir;
      dg.Ng = ray.Ng;
      dg.Ns = ray.Ng;
      int materialID = postIntersect(ray,dg);
      dg.Ng = face_forward(ray.dir,normalize(dg.Ng));
      dg.Ns = face_forward(ray.dir,normalize(dg.Ns));
      
      /* shade */
      if (g_ispc_scene->materials[materialID]->type == MATERIAL_OBJ) {
        ISPCOBJMaterial* material = (ISPCOBJMaterial*) g_ispc_scene->materials[materialID];
        Kd = Vec3fa(material->Kd);
      }
      
      color = Kd*dot(neg(ray.dir),dg.Ns);
      //color = Vec3fa(abs(dot(ray.dir,normalize(ray.Ng))));
    }
#else
      color = ambientOcclusionShading(x,y,ray,g_stats[threadIndex]);
#endif

    /* write color to framebuffer */
    unsigned int r = (unsigned int) (255.0f * clamp(color.x,0.0f,1.0f));
    unsigned int g = (unsigned int) (255.0f * clamp(color.y,0.0f,1.0f));
    unsigned int b = (unsigned int) (255.0f * clamp(color.z,0.0f,1.0f));
    pixels[y*width+x] = (b << 16) + (g << 8) + r;
  }
}

/* task that renders a single screen tile */
void renderTileTask (int taskIndex, int threadIndex, int* pixels,
                         const unsigned int width,
                         const unsigned int height,
                         const float time,
                         const ISPCCamera& camera,
                         const int numTilesX,
                         const int numTilesY)
{
  renderTile(taskIndex,threadIndex,pixels,width,height,time,camera,numTilesX,numTilesY);
}

/* called by the C++ code for initialization */
extern "C" void device_init (char* cfg)
{
  /* create new Embree device */
  g_device = rtcNewDevice(cfg);
  error_handler(nullptr,rtcDeviceGetError(g_device));

  /* set error handler */
  rtcDeviceSetErrorFunction(g_device,error_handler,nullptr);

  /* set render tile function to use */
  renderTile = renderTileStandard;
  key_pressed_handler = device_key_pressed_default;
}

/* called by the C++ code to render */
extern "C" void device_render (int* pixels,
                           const unsigned int width,
                           const unsigned int height,
                           const float time,
                           const ISPCCamera& camera)
{
  /* create scene */
  if (!g_scene) {
    g_scene = convertScene(g_ispc_scene);
    updateEdgeLevels(g_ispc_scene, camera.xfm.p);
    rtcCommit (g_scene);
  }
  
  /* render image */
  const int numTilesX = (width +TILE_SIZE_X-1)/TILE_SIZE_X;
  const int numTilesY = (height+TILE_SIZE_Y-1)/TILE_SIZE_Y;
  parallel_for(size_t(0),size_t(numTilesX*numTilesY),[&](const range<size_t>& range) {
    const int threadIndex = (int)TaskScheduler::threadIndex();
    for (size_t i=range.begin(); i<range.end(); i++)
      renderTileTask((int)i,threadIndex,pixels,width,height,time,camera,numTilesX,numTilesY);
  }); 
}

/* called by the C++ code for cleanup */
extern "C" void device_cleanup ()
{
  rtcDeleteScene (g_scene); g_scene = nullptr;
  rtcDeleteDevice(g_device); g_device = nullptr;
}

} // namespace embree
