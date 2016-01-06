/*!
  @file
  @author Shin'ichiro Nakaoka
*/

#include "GLSceneRenderer.h"
#include <cnoid/SceneDrawables>
#include <cnoid/SceneCameras>
#include <cnoid/SceneLights>
#include <cnoid/SceneEffects>
#include <cnoid/EigenUtil>
#include <Eigen/StdVector>
#include <boost/variant.hpp>
#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include <GL/gl.h>

using namespace std;
using namespace cnoid;

namespace {

struct PreproNode
{
    PreproNode() : parent(0), child(0), next(0) { }
    ~PreproNode() {
        if(child) delete child;
        if(next) delete next;
    }
    enum { GROUP, TRANSFORM, PREPROCESSED, LIGHT, FOG, CAMERA };
    boost::variant<SgGroup*, SgTransform*, SgPreprocessed*, SgLight*, SgFog*, SgCamera*> node;
    SgNode* base;
    PreproNode* parent;
    PreproNode* child;
    PreproNode* next;
    template<class T> void setNode(T* n){
        node = n;
        base = n;
    }
};
    

class PreproTreeExtractor : public SceneVisitor
{
    PreproNode* node;
    bool found;
public:
    PreproNode* apply(SgNode* node);
    virtual void visitGroup(SgGroup* group);
    virtual void visitTransform(SgTransform* transform);
    virtual void visitShape(SgShape* shape);
    virtual void visitPointSet(SgPointSet* pointSet);        
    virtual void visitLineSet(SgLineSet* lineSet);        
    virtual void visitPreprocessed(SgPreprocessed* preprocessed);
    virtual void visitLight(SgLight* light);
    virtual void visitFog(SgFog* fog);
    virtual void visitCamera(SgCamera* camera);
};

}


PreproNode* PreproTreeExtractor::apply(SgNode* snode)
{
    node = 0;
    found = false;
    snode->accept(*this);
    return node;
}


void PreproTreeExtractor::visitGroup(SgGroup* group)
{
    bool foundInSubTree = false;

    PreproNode* self = new PreproNode();
    self->setNode(group);

    for(SgGroup::const_reverse_iterator p = group->rbegin(); p != group->rend(); ++p){
        
        node = 0;
        found = false;
        
        (*p)->accept(*this);
        
        if(node){
            if(found){
                node->parent = self;
                node->next = self->child;
                self->child = node;
                foundInSubTree = true;
            } else {
                delete node;
            }
        }
    }
            
    found = foundInSubTree;

    if(found){
        node = self;
    } else {
        delete self;
        node = 0;
    }
}


void PreproTreeExtractor::visitTransform(SgTransform* transform)
{
    visitGroup(transform);
    if(node){
        node->setNode(transform);
    }
}


void PreproTreeExtractor::visitShape(SgShape* shape)
{

}


void PreproTreeExtractor::visitPointSet(SgPointSet* shape)
{

}


void PreproTreeExtractor::visitLineSet(SgLineSet* shape)
{

}


void PreproTreeExtractor::visitPreprocessed(SgPreprocessed* preprocessed)
{
    node = new PreproNode();
    node->setNode(preprocessed);
    found = true;
}


void PreproTreeExtractor::visitLight(SgLight* light)
{
    node = new PreproNode();
    node->setNode(light);
    found = true;
}


void PreproTreeExtractor::visitFog(SgFog* fog)
{
    node = new PreproNode();
    node->setNode(fog);
    found = true;
}
    

void PreproTreeExtractor::visitCamera(SgCamera* camera)
{
    node = new PreproNode();
    node->setNode(camera);
    found = true;
}


namespace cnoid {

class GLSceneRendererImpl
{
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
        
    GLSceneRendererImpl(GLSceneRenderer* self, SgGroup* sceneRoot);
    ~GLSceneRendererImpl();

    GLSceneRenderer* self;

    Signal<void()> sigRenderingRequest;

    SgGroupPtr sceneRoot;
    SgGroupPtr scene;

    bool doPreprocessedNodeTreeExtraction;
    boost::scoped_ptr<PreproNode> preproTree;

    Array4i viewport;

    struct CameraInfo
    {
        EIGEN_MAKE_ALIGNED_OPERATOR_NEW

        CameraInfo() : camera(0), M(Affine3::Identity()), node(0) { }
        CameraInfo(SgCamera* camera, const Affine3& M, PreproNode* node)
            : camera(camera), M(M), node(node) { }
        SgCamera* camera;

        // I want to use 'T' here, but that causes a compile error (c2327) for VC++2010.
        // 'T' is also used as a template parameter in a template code of Eigen,
        // and it seems that the name of a template parameter and this member conflicts.
        // So I changed the name to 'M'.
        // This behavior of VC++ seems stupid!!!
        Affine3 M;
        PreproNode* node;
    };

    typedef vector<CameraInfo, Eigen::aligned_allocator<CameraInfo> > CameraInfoArray;
    CameraInfoArray cameras1;
    CameraInfoArray cameras2;
    CameraInfoArray* cameras;
    CameraInfoArray* prevCameras;

    bool camerasChanged;
    bool currentCameraRemoved;
    int currentCameraIndex;
    SgCamera* currentCamera;
    vector<SgNodePath> cameraPaths;
    Signal<void()> sigCamerasChanged;
    Signal<void()> sigCurrentCameraChanged;
        
    GLfloat aspectRatio; // width / height;

    struct LightInfo
    {
        EIGEN_MAKE_ALIGNED_OPERATOR_NEW

        LightInfo() : light(0) { }
        LightInfo(SgLight* light, const Affine3& M) : light(light), M(M) { } 
        SgLight* light;
        Affine3 M;
    };
    vector<LightInfo, Eigen::aligned_allocator<LightInfo> > lights;

    SgLightPtr headLight;
    bool isHeadLightLightingFromBackEnabled;
    std::set<SgLightPtr> defaultLights;
    bool additionalLightsEnabled;

    SgFogPtr currentFog;

    void onSceneGraphUpdated(const SgUpdate& update);
    void updateCameraPaths();
    void setCurrentCamera(int index, bool doRenderingRequest);
    bool setCurrentCamera(SgCamera* camera);
    void setViewport(int x, int y, int width, int height);
    void extractPreproNodes();
    void extractPreproNodeIter(PreproNode* node, const Affine3& T);
};

}


GLSceneRenderer::GLSceneRenderer()
{
    SgGroup* sceneRoot = new SgGroup;
    sceneRoot->setName("Root");
    impl = new GLSceneRendererImpl(this, sceneRoot);
}


GLSceneRenderer::GLSceneRenderer(SgGroup* sceneRoot)
{
    impl = new GLSceneRendererImpl(this, sceneRoot);
}


GLSceneRendererImpl::GLSceneRendererImpl(GLSceneRenderer* self, SgGroup* sceneRoot)
    : self(self),
      sceneRoot(sceneRoot)
{
    sceneRoot->sigUpdated().connect(boost::bind(&GLSceneRendererImpl::onSceneGraphUpdated, this, _1));

    doPreprocessedNodeTreeExtraction = true;

    aspectRatio = 1.0f;

    cameras = &cameras1;
    prevCameras = &cameras2;
    currentCameraIndex = -1;
    currentCamera = 0;

    headLight = new SgDirectionalLight();
    headLight->setAmbientIntensity(0.0f);
    isHeadLightLightingFromBackEnabled = false;
    additionalLightsEnabled = true;

    scene = new SgGroup();
    sceneRoot->addChild(scene);
}


GLSceneRenderer::~GLSceneRenderer()
{
    delete impl;
}


GLSceneRendererImpl::~GLSceneRendererImpl()
{

}


SgGroup* GLSceneRenderer::sceneRoot()
{
    return impl->sceneRoot;
}


SgGroup* GLSceneRenderer::scene()
{
    return impl->scene;
}


void GLSceneRenderer::clearScene()
{
    impl->scene->clearChildren(true);
}


void GLSceneRendererImpl::onSceneGraphUpdated(const SgUpdate& update)
{
    if(update.action() & (SgUpdate::ADDED | SgUpdate::REMOVED)){
        doPreprocessedNodeTreeExtraction = true;
    }
    if(SgImage* image = dynamic_cast<SgImage*>(update.path().front())){
        self->onImageUpdated(image);
    }
    sigRenderingRequest();
}


SignalProxy<void()> GLSceneRenderer::sigRenderingRequest()
{
    return impl->sigRenderingRequest;
}


int GLSceneRenderer::numCameras() const
{
    return impl->cameras->size();
}


SgCamera* GLSceneRenderer::camera(int index)
{
    return dynamic_cast<SgCamera*>(cameraPath(index).back());
}


const std::vector<SgNode*>& GLSceneRenderer::cameraPath(int index) const
{
    if(impl->cameraPaths.empty()){
        impl->updateCameraPaths();
    }
    return impl->cameraPaths[index];
}


void GLSceneRendererImpl::updateCameraPaths()
{
    vector<SgNode*> tmpPath;
    const int n = cameras->size();
    cameraPaths.resize(n);
    
    for(int i=0; i < n; ++i){
        CameraInfo& info = (*cameras)[i];
        tmpPath.clear();
        PreproNode* node = info.node;
        while(node){
            tmpPath.push_back(node->base);
            node = node->parent;
        }
        if(!tmpPath.empty()){
            tmpPath.pop_back(); // remove the root node
            vector<SgNode*>& path = cameraPaths[i];
            path.resize(tmpPath.size());
            std::copy(tmpPath.rbegin(), tmpPath.rend(), path.begin());
        }
    }
}


SignalProxy<void()> GLSceneRenderer::sigCamerasChanged() const
{
    return impl->sigCamerasChanged;
}


void GLSceneRenderer::setCurrentCamera(int index)
{
    impl->setCurrentCamera(index, true);
}


void GLSceneRendererImpl::setCurrentCamera(int index, bool doRenderingRequest)
{
    SgCamera* newCamera = 0;
    if(index >= 0 && index < cameras->size()){
        newCamera = (*cameras)[index].camera;
    }
    if(newCamera && newCamera != currentCamera){
        currentCameraIndex = index;
        currentCamera = newCamera;
        sigCurrentCameraChanged();
        if(doRenderingRequest){
            sigRenderingRequest();
        }
    }
}


bool GLSceneRenderer::setCurrentCamera(SgCamera* camera)
{
    return impl->setCurrentCamera(camera);
}


bool GLSceneRendererImpl::setCurrentCamera(SgCamera* camera)
{
    if(camera != currentCamera){
        for(size_t i=0; i < cameras->size(); ++i){
            if((*cameras)[i].camera == camera){
                setCurrentCamera(i, true);
                return true;
            }
        }
    }
    return false;
}


SgCamera* GLSceneRenderer::currentCamera() const
{
    return impl->currentCamera;
}


int GLSceneRenderer::currentCameraIndex() const
{
    return impl->currentCameraIndex;
}


SignalProxy<void()> GLSceneRenderer::sigCurrentCameraChanged()
{
    return impl->sigCurrentCameraChanged;
}


void GLSceneRenderer::setViewport(int x, int y, int width, int height)
{
    impl->setViewport(x, y, width, height);
}


void GLSceneRendererImpl::setViewport(int x, int y, int width, int height)
{
    aspectRatio = (double)width / height;
    viewport << x, y, width, height;
    glViewport(x, y, width, height);
}


Array4i GLSceneRenderer::viewport() const
{
    return impl->viewport;
}


void GLSceneRenderer::getViewport(int& out_x, int& out_y, int& out_width, int& out_height) const
{
    out_x = impl->viewport[0];
    out_y = impl->viewport[1];
    out_width = impl->viewport[2];
    out_height = impl->viewport[3];
}    


double GLSceneRenderer::aspectRatio() const
{
    return impl->aspectRatio;
}


void GLSceneRenderer::extractPreproNodes()
{
    impl->extractPreproNodes();
}


void GLSceneRendererImpl::extractPreproNodes()
{
    if(doPreprocessedNodeTreeExtraction){
        PreproTreeExtractor extractor;
        preproTree.reset(extractor.apply(sceneRoot));
        doPreprocessedNodeTreeExtraction = false;
    }

    std::swap(cameras, prevCameras);
    cameras->clear();
    camerasChanged = false;
    currentCameraRemoved = true;
    
    lights.clear();
    currentFog = 0;

    if(preproTree){
        extractPreproNodeIter(preproTree.get(), Affine3::Identity());
    }

    if(!camerasChanged){
        if(cameras->size() != prevCameras->size()){
            camerasChanged = true;
        }
    }
    if(camerasChanged){
        if(currentCameraRemoved){
            currentCameraIndex = 0;
        }
        cameraPaths.clear();
        sigCamerasChanged();
    }

    setCurrentCamera(currentCameraIndex, false);
}


void GLSceneRendererImpl::extractPreproNodeIter(PreproNode* node, const Affine3& T)
{
    switch(node->node.which()){

    case PreproNode::GROUP:
        for(PreproNode* childNode = node->child; childNode; childNode = childNode->next){
            extractPreproNodeIter(childNode, T);
        }
        break;
        
    case PreproNode::TRANSFORM:
    {
        SgTransform* transform = boost::get<SgTransform*>(node->node);
        Affine3 T1;
        transform->getTransform(T1);
        const Affine3 T2 = T * T1;
        for(PreproNode* childNode = node->child; childNode; childNode = childNode->next){
            extractPreproNodeIter(childNode, T2);
        }
    }
    break;
        
    case PreproNode::PREPROCESSED:
        // call additional functions
        break;

    case PreproNode::LIGHT:
    {
        SgLight* light = boost::get<SgLight*>(node->node);
        if(additionalLightsEnabled || defaultLights.find(light) != defaultLights.end()){
            lights.push_back(LightInfo(light, T));
        }
        break;
    }

    case PreproNode::FOG:
    {
        SgFog* fog = boost::get<SgFog*>(node->node);
        currentFog = fog;
        break;
    }

    case PreproNode::CAMERA:
    {
        SgCamera* camera = boost::get<SgCamera*>(node->node);
        size_t index = cameras->size();
        if(!camerasChanged){
            if(index >= prevCameras->size() || camera != (*prevCameras)[index].camera){
                camerasChanged = true;
            }
        }
        if(camera == currentCamera){
            currentCameraRemoved = false;
            currentCameraIndex = cameras->size();
        }
        cameras->push_back(CameraInfo(camera, T, node));
    }
    break;

    default:
        break;
    }
}

void GLSceneRenderer::flush()
{
    glFlush();
}


SgLight* GLSceneRenderer::headLight()
{
    return impl->headLight.get();
}


void GLSceneRenderer::setHeadLight(SgLight* light)
{
    impl->headLight = light;
}


void GLSceneRenderer::setHeadLightLightingFromBackEnabled(bool on)
{
    impl->isHeadLightLightingFromBackEnabled = on;
}


void GLSceneRenderer::setAsDefaultLight(SgLight* light)
{
    impl->defaultLights.insert(light);
}


void GLSceneRenderer::unsetDefaultLight(SgLight* light)
{
    impl->defaultLights.erase(light);
}


void GLSceneRenderer::enableAdditionalLights(bool on)
{
    impl->additionalLightsEnabled = on;
}


void GLSceneRenderer::getViewFrustum
(const SgPerspectiveCamera& camera, double& left, double& right, double& bottom, double& top) const
{
    top = camera.nearDistance() * tan(camera.fovy(impl->aspectRatio) / 2.0);
    bottom = -top;
    right = top * impl->aspectRatio;
    left = -right;
}


void GLSceneRenderer::getViewVolume
(const SgOrthographicCamera& camera, float& out_left, float& out_right, float& out_bottom, float& out_top) const
{
    float h = camera.height();
    out_top = h / 2.0f;
    out_bottom = -h / 2.0f;
    float w = h * impl->aspectRatio;
    out_left = -w / 2.0f;
    out_right = w / 2.0f;
}
