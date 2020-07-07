#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Optional.h>

#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/DefaultFramebuffer.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/MeshTools/Interleave.h>
#include <Magnum/MeshTools/CompressIndices.h>
#include <Magnum/Platform/Sdl2Application.h>
#include <Magnum/Primitives/Cube.h>
#include <Magnum/Primitives/Axis.h>
#include <Magnum/Primitives/Plane.h>
#include <Magnum/Shaders/Phong.h>
#include <Magnum/Shaders/Flat.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/SceneGraph/SceneGraph.h>
#include <Magnum/SceneGraph/MatrixTransformation3D.h>
#include <Magnum/SceneGraph/Camera.h>
#include <Magnum/SceneGraph/Drawable.h>
#include <Magnum/SceneGraph/Scene.h>
#include <Magnum/MeshTools/Compile.h>
#include <Magnum/GL/RenderbufferFormat.h>
#include <Magnum/GL/Version.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/GL/Renderbuffer.h>
#include <Magnum/GL/Context.h>

#include <util/tiny_logger.hpp>
#include <env/env.hpp>


using namespace Magnum;
using namespace Magnum::Math::Literals;


typedef SceneGraph::Object<SceneGraph::MatrixTransformation3D> Object3D;
typedef SceneGraph::Scene<SceneGraph::MatrixTransformation3D> Scene3D;


struct InstanceData {
    Matrix4 transformationMatrix;
    Matrix3x3 normalMatrix;
    Color3 color;
};


class CustomDrawable : public SceneGraph::Drawable3D
{
public:
    explicit CustomDrawable(
        Object3D &parentObject,
        Containers::Array<InstanceData> &instanceData,
        const Color3 &color,
        const Matrix4 &primitiveTransformation,
        SceneGraph::DrawableGroup3D &drawables
    )
        : SceneGraph::Drawable3D{parentObject, &drawables}, _instanceData(instanceData), _color{color},
          _primitiveTransformation{primitiveTransformation}
    {}

private:
    void draw(const Matrix4& transformation, SceneGraph::Camera3D&) override {
        const Matrix4 t = transformation*_primitiveTransformation;
        arrayAppend(_instanceData, Containers::InPlaceInit, t, t.normalMatrix(), _color);
    }

    Containers::Array<InstanceData>& _instanceData;
    Color3 _color;
    Matrix4 _primitiveTransformation;
};


class SimpleDrawable3D : public SceneGraph::Drawable3D
{
public:
    explicit SimpleDrawable3D(Object3D &parentObject, SceneGraph::DrawableGroup3D &drawables,
        Shaders::Phong & shader, GL::Mesh& mesh)
        : SceneGraph::Drawable3D{parentObject, &drawables}
        , _shader(shader), _mesh(mesh)
    {}

    void draw(const Matrix4& transformationMatrix, SceneGraph::Camera3D& camera) override {
        _shader.setTransformationMatrix(transformationMatrix)
            .setNormalMatrix(transformationMatrix.normalMatrix())
            .setProjectionMatrix(camera.projectionMatrix())
            .draw(_mesh);
    }

private:
    Shaders::Phong& _shader;
    GL::Mesh& _mesh;
};


class FlatDrawable : public SceneGraph::Drawable3D
{
public:
    explicit FlatDrawable(Object3D &parentObject, SceneGraph::DrawableGroup3D &drawables,
                          Shaders::Flat3D & shader, GL::Mesh& mesh)
        : SceneGraph::Drawable3D{parentObject, &drawables}
        , _shader(shader), _mesh(mesh)
    {}

    void draw(const Matrix4& transformation, SceneGraph::Camera3D& camera) override
    {
        _shader.setTransformationProjectionMatrix(camera.projectionMatrix() * transformation).draw(_mesh);
    }

private:
    Shaders::Flat3D& _shader;
    GL::Mesh& _mesh;
};



class Viewer: public Platform::Application
{
public:
    explicit Viewer(const Arguments& arguments);

private:
    void drawEvent() override;

    void tickEvent() override;

    void mouseScrollEvent(MouseScrollEvent& event) override;

    void keyPressEvent(KeyEvent &event) override;

private:
    Env env;

    const std::vector<BoundingBox> &layoutDrawables = env.getLayoutDrawables();

    std::vector<std::unique_ptr<Object3D>> layoutObjects;
    std::vector<std::unique_ptr<CustomDrawable>> instancedLayoutDrawables;

    std::unique_ptr<Object3D> axisObject;
    std::unique_ptr<FlatDrawable> axisDrawable;

    std::unique_ptr<Object3D> exitPadObject;
    std::unique_ptr<SimpleDrawable3D> exitPadDrawable;

    GL::Buffer voxelInstanceBuffer{NoCreate};
    Containers::Array<InstanceData> voxelInstanceData;

    Scene3D _scene;
    Object3D* cameraObject;
    SceneGraph::Camera3D* camera;
    SceneGraph::DrawableGroup3D drawables;

    Shaders::Phong shader{NoCreate};
    Shaders::Phong shaderInstanced{NoCreate};
    Shaders::Flat3D flatShader{NoCreate};

    GL::Framebuffer framebuffer;
    GL::Renderbuffer colorBuffer, depthBuffer;

    GL::Mesh cubeMesh, axis, exitPadMesh;

    int direction = -1;
};

Viewer::Viewer(const Arguments& arguments):
    Platform::Application{arguments, Configuration{}.setTitle("Magnum test")}, framebuffer{GL::defaultFramebuffer.viewport()}
{
    MAGNUM_ASSERT_GL_VERSION_SUPPORTED(GL::Version::GL330);
    GL::Renderer::enable(GL::Renderer::Feature::DepthTest);
    GL::Renderer::enable(GL::Renderer::Feature::FaceCulling);

    colorBuffer.setStorage(GL::RenderbufferFormat::SRGB8Alpha8, GL::defaultFramebuffer.viewport().size());
    depthBuffer.setStorage(GL::RenderbufferFormat::DepthComponent24, GL::defaultFramebuffer.viewport().size());

    framebuffer.attachRenderbuffer(GL::Framebuffer::ColorAttachment{0}, colorBuffer);
    framebuffer.attachRenderbuffer(GL::Framebuffer::BufferAttachment::Depth, depthBuffer);
    framebuffer.mapForDraw({
        {Shaders::Phong::ColorOutput, GL::Framebuffer::ColorAttachment{0}},
    });

    CORRADE_INTERNAL_ASSERT(framebuffer.checkStatus(GL::FramebufferTarget::Draw) == GL::Framebuffer::Status::Complete);

    shader = Shaders::Phong{Shaders::Phong::Flag::VertexColor};
    shader.setAmbientColor(0x111111_rgbf).setSpecularColor(0x330000_rgbf).setLightPosition({10.0f, 15.0f, 5.0f});

    shaderInstanced = Shaders::Phong{Shaders::Phong::Flag::VertexColor|Shaders::Phong::Flag::InstancedTransformation};
    shaderInstanced.setAmbientColor(0x111111_rgbf).setSpecularColor(0x330000_rgbf).setLightPosition({10.0f, 15.0f, 5.0f});

    flatShader = Shaders::Flat3D {};
    flatShader.setColor(0xffffff_rgbf);

    axis = MeshTools::compile(Primitives::axis3D());

    axisObject = std::make_unique<Object3D>(&_scene);
    axisObject->scale(Vector3{3, 3, 3});
    axisDrawable = std::make_unique<FlatDrawable>(*axisObject, drawables, flatShader, axis);

    exitPadMesh = MeshTools::compile(Primitives::planeSolid());
    exitPadObject = std::make_unique<Object3D>(&_scene);
    const auto exitPadCoords = env.getExitPadCoords();
    const Vector3 exitPadPos(exitPadCoords.min.x(), exitPadCoords.min.y(), exitPadCoords.min.z());
    exitPadObject->rotateX(-90.0_degf).scale({0.5, 0.5, 0.5}).translate({0.5, 0.05, 0.5});
    exitPadObject->translate(exitPadPos);
    exitPadDrawable = std::make_unique<SimpleDrawable3D>(*exitPadObject, drawables, shader, exitPadMesh);

    cubeMesh = MeshTools::compile(Primitives::cubeSolid());
    voxelInstanceBuffer = GL::Buffer{};
    cubeMesh.addVertexBufferInstanced(
        voxelInstanceBuffer, 1, 0,
        Shaders::Phong::TransformationMatrix{},
        Shaders::Phong::NormalMatrix{},
        Shaders::Phong::Color3{}
    );

    for (auto layoutDrawable : layoutDrawables) {
        auto voxelObject = std::make_unique<Object3D>(&_scene);

        const auto bboxMin = layoutDrawable.min, bboxMax = layoutDrawable.max;
        auto scale = Vector3{
            float(bboxMax.x() - bboxMin.x() + 1) / 2,
            float(bboxMax.y() - bboxMin.y() + 1) / 2,
            float(bboxMax.z() - bboxMin.z() + 1) / 2,
        };
        TLOG(INFO) << scale.x() << " " << scale.y() << " " << scale.z();

        voxelObject->scale(scale).translate({0.5, 0.5, 0.5}).translate({
            float((bboxMin.x() + bboxMax.x())) / 2,
            float((bboxMin.y() + bboxMax.y())) / 2,
            float((bboxMin.z() + bboxMax.z())) / 2,
        });

        auto transformation = Matrix4::scaling(Vector3{1.0f});

        auto voxel = std::make_unique<CustomDrawable>(
            *voxelObject, voxelInstanceData, 0xa5c9ea_rgbf, transformation, drawables
        );

        layoutObjects.emplace_back(std::move(voxelObject));
        instancedLayoutDrawables.emplace_back(std::move(voxel));
    }

    /* Configure camera */
    cameraObject = new Object3D{&_scene};
    cameraObject->rotateX(0.0_degf);
    cameraObject->rotateY(250.0_degf);
    cameraObject->translate(Vector3{1.5, 3, 1.5});
    camera = new SceneGraph::Camera3D{*cameraObject};
    camera->setAspectRatioPolicy(SceneGraph::AspectRatioPolicy::Extend)
        .setProjectionMatrix(Matrix4::perspectiveProjection(60.0_degf, 4.0f/3.0f, 0.1f, 50.0f))
        .setViewport(GL::defaultFramebuffer.viewport().size());
}


void Viewer::mouseScrollEvent(MouseScrollEvent& event) {
    if(!event.offset().y()) return;

    /* Distance to origin */
    const Float distance = cameraObject->transformation().translation().z();

    /* Move 15% of the distance back or forward */
    cameraObject->translate(Vector3::zAxis(distance * (1.0f - (event.offset().y() > 0 ? 1 / 0.85f : 0.85f))));

    redraw();
}

void Viewer::drawEvent() {
    framebuffer
        .clearColor(0, Color3{0.125f})
        .clearColor(1, Vector4ui{})
        .clearDepth(1.0f)
        .bind();

    arrayResize(voxelInstanceData, 0);
    camera->draw(drawables);

    shaderInstanced.setProjectionMatrix(camera->projectionMatrix());

    /* Upload instance data to the GPU (orphaning the previous buffer
       contents) and draw all cubes in one call, and all spheres (if any)
       in another call */
    voxelInstanceBuffer.setData(voxelInstanceData, GL::BufferUsage::DynamicDraw);
    cubeMesh.setInstanceCount(voxelInstanceData.size());
    shaderInstanced.draw(cubeMesh);

    /* Bind the main buffer back */
    GL::defaultFramebuffer.clear(GL::FramebufferClear::Color|GL::FramebufferClear::Depth).bind();

    /* Blit color to window framebuffer */
    framebuffer.mapForRead(GL::Framebuffer::ColorAttachment{0});
    GL::AbstractFramebuffer::blit(framebuffer, GL::defaultFramebuffer,
                                  {{}, framebuffer.viewport().size()}, GL::FramebufferBlit::Color);

    swapBuffers();
}

void Viewer::tickEvent() {
//    cameraObject->translate(Vector3{float(direction) * 0.001f, 0, 0});
////    TLOG(DEBUG) << "tick " << cameraObject->transformation().translation().x();
//
//    if (abs(cameraObject->transformation().translation().x()) > 18)
//        direction *= -1;

    redraw();
}

void Viewer::keyPressEvent(KeyEvent& event)
{
//    camera.rotate(-glm::vec2(yDelta, xDelta) * angularVelocity);

    constexpr auto step = 0.5f;
    constexpr auto turn = 5.0_degf;

    switch(event.key()) {
        case KeyEvent::Key::W:
            cameraObject->translate(-step * cameraObject->transformation().backward());
            break;
        case KeyEvent::Key::S:
            cameraObject->translate(step * cameraObject->transformation().backward());
            break;
        case KeyEvent::Key::A:
        case KeyEvent::Key::Left:
            cameraObject->rotateYLocal(turn);
            break;
        case KeyEvent::Key::D:
        case KeyEvent::Key::Right:
            cameraObject->rotateYLocal(-turn);
            break;
        case KeyEvent::Key::Up:
            cameraObject->rotateXLocal(turn);
            break;
        case KeyEvent::Key::Down:
            cameraObject->rotateXLocal(-turn);
            break;
        default:
            return;
    }

    event.setAccepted();
    redraw(); /* camera has changed, redraw! */
}

MAGNUM_APPLICATION_MAIN(Viewer)