#include <atomic>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <thread>

#include <boost/asio/io_service.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <cassert>
#include <cstdlib>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace {
  template <typename Arg>
  void perror(Arg&& message) {
    static constexpr std::size_t errno_buffer_size = 256;
    char buffer[errno_buffer_size];
    strerror_r(errno, buffer, errno_buffer_size);
    std::cerr << message << ": " << buffer << std::endl;
  }

  class FileDescriptor final {
  private:
    int mHandle;
  public:
    FileDescriptor() : mHandle{-1} {}
    FileDescriptor(char const *path)
      : mHandle{open(path, O_RDWR)}
    {
      if (!mHandle) perror("Couldn't open file");
    }

    FileDescriptor(FileDescriptor const &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept
      : mHandle{other.mHandle}
    {
      other.mHandle = -1;
    }
    FileDescriptor &operator=(FileDescriptor const &) = delete;
    FileDescriptor &operator=(FileDescriptor &&other) noexcept {
      // This class is final, and nothing here can throw exceptions.
      if (&other == this) return *this;
      this->~FileDescriptor();
      new (this) FileDescriptor{std::move(other)};
      return *this;
    }
    ~FileDescriptor() {
      if (*this) close(mHandle);
    }

    explicit operator bool() const {
      return mHandle >= 0;
    }

    int get() const {
      assert(*this);
      return mHandle;
    }
  };

  template <typename T>
  class Span final {
  private:
    T *mBegin;
    T *mEnd;
  public:
    template <typename Size>
    Span(T *begin, Size count)
      : mBegin{begin}
      , mEnd{begin + count}
    {}
    T *begin() { return mBegin; }
    T *end() { return mEnd; }
    T const *begin() const { return mBegin; }
    T const *end() const { return mEnd; }
  };

  namespace drm {
    class Descriptor final {
    private:
      FileDescriptor mFile;
      Descriptor(FileDescriptor file) : mFile{std::move(file)} {}
    public:
      Descriptor() = default;
      static Descriptor create(char const *path) {
        FileDescriptor file{path};
        if (!file) return {};

        int error = drmSetMaster(file.get());
        if (error) {
          perror("Couldn't become drm master!");
          return {};
        }

        return {std::move(file)};
      }
      Descriptor(Descriptor const &) = delete;
      Descriptor &operator=(Descriptor const &) = delete;
      Descriptor(Descriptor &&) = default;
      Descriptor &operator=(Descriptor &&) = default;
      ~Descriptor() {
        if (!*this) return;
        int error = drmDropMaster(mFile.get());
        if (error) perror("Error dropping drm master!");
      }

      explicit operator bool() const { return static_cast<bool>(mFile); }

      int get() const { return mFile.get(); }
    };

    class Encoder final {
    private:
      // I'm not sure what guarantees we have -- better safe than sorry
      static void safe_delete(drmModeEncoder *encoder) {
        if (encoder != nullptr) drmModeFreeEncoder(encoder);
      }
      std::unique_ptr<drmModeEncoder, decltype(&safe_delete)> mHandle;
    public:
      Encoder() : mHandle{nullptr, &safe_delete} {}
      Encoder(drm::Descriptor const &gpu, uint32_t encoder_id)
        : mHandle{
            drmModeGetEncoder(gpu.get(), encoder_id)
          , &safe_delete
          }
      { if (!mHandle) perror("Couldn't get encoder"); }

      explicit operator bool() const { return mHandle != nullptr; }

      uint32_t id() const { return mHandle->encoder_id; }

      uint32_t crtc_id() const { return mHandle->crtc_id; }

      bool has_crtc(int index) const {
        return mHandle->possible_crtcs & (1 << index);
      }
    };

    class Connector final {
    private:
      // I'm not sure what guarantees we have -- better safe than sorry
      static void safe_delete(drmModeConnector *connector) {
        if (connector != nullptr) drmModeFreeConnector(connector);
      }
      std::unique_ptr<drmModeConnector, decltype(&safe_delete)>
        mHandle
      ;
    public:
      Connector() : mHandle{nullptr, &safe_delete} {}
      Connector(drm::Descriptor const &gpu, uint32_t connector_id)
        : mHandle{
            drmModeGetConnector(gpu.get(), connector_id)
          , &safe_delete
          }
      { if (!mHandle) perror("Couldn't get connector"); }

      explicit operator bool() const { return mHandle != nullptr; }

      bool is_connected() const {
        assert(*this);
        return mHandle->connection == DRM_MODE_CONNECTED;
      }

      uint32_t id() const {
        assert(*this);
        return mHandle->connector_id;
      }

      uint32_t encoder_id() const {
        assert(*this);
        return mHandle->encoder_id;
      }

      drmModeModeInfo *find_best_mode() const {
        assert(*this);
        drmModeModeInfo *result = nullptr;
        for (int i = 0, biggest_area = 0; i < mHandle->count_modes; i++) {
          drmModeModeInfo &mode = mHandle->modes[i];
          if (mode.type & DRM_MODE_TYPE_PREFERRED) return &mode;
          int area = mode.hdisplay * mode.vdisplay;
          if (area > biggest_area) {
            result = &mode;
            biggest_area = area;
          }
        }
        if (!result) std::cerr << "No mode found" << std::endl;
        return result;
      }

      Span<uint32_t> const encoders() const {
        assert(*this);
        return {mHandle->encoders, mHandle->count_encoders};
      }
    };

    class Resources final {
    private:
      // I'm not sure what guarantees we have -- better safe than sorry
      static void safe_delete(drmModeRes *resources) {
        if (resources != nullptr) drmModeFreeResources(resources);
      }
      std::unique_ptr<drmModeRes, decltype(&safe_delete)> mHandle;

    public:
      Resources(drm::Descriptor const &gpu)
        : mHandle{drmModeGetResources(gpu.get()), &safe_delete}
      { if (!mHandle) perror("Couldn't retrieve DRM resources"); }

      explicit operator bool() const { return mHandle != nullptr; }

      Span<uint32_t> const connectors() const {
        assert(*this);
        return {mHandle->connectors, mHandle->count_connectors};
      }

      Span<uint32_t> const crtcs() const {
        assert(*this);
        return {mHandle->crtcs, mHandle->count_crtcs};
      }

      Span<uint32_t> const encoders() const {
        assert(*this);
        return {mHandle->encoders, mHandle->count_encoders};
      }
    };

    class FrameBuffer final {
    private:
      class Handle {
      private:
        int mGPUDescriptor;
        uint32_t mFrameBufferHandle;
      public:
        uint32_t get() const { return mFrameBufferHandle; }
        Handle(int gpu, uint32_t framebuffer)
          : mGPUDescriptor{gpu}, mFrameBufferHandle{framebuffer}
        {}
        ~Handle() {
          drmModeRmFB(mGPUDescriptor, mFrameBufferHandle);
        }
      };
      Handle mHandle;

      FrameBuffer(int gpu, uint32_t framebuffer)
        : mHandle{gpu, framebuffer}
      {}
    public:
      FrameBuffer() = default;
      // This is heap allocated to interact with the gbm C API. Note that it
      // keeps a reference to the gpu descriptor, so its use should be limited
      // to the scope of the owner of the gpu descriptor.
      static FrameBuffer *create(
        drm::Descriptor const &gpu
      , uint32_t width, uint32_t height
      , uint32_t pitch, uint32_t bo_handle
      ) {
        assert(gpu);

        constexpr uint8_t depth = 24;
        constexpr uint8_t pixel_bits = 32;
        uint32_t framebuffer_id;
        // Note that there are more variants of this function
        // (currently drmModeAddFB2 and drmModeAddFB2WithModifiers)
        int error = drmModeAddFB(
          gpu.get()
        , width, height
        , depth, pixel_bits, pitch
        , bo_handle, &framebuffer_id
        );
        if (error) {
          perror("Failed to create framebuffer");
          return nullptr;
        }

        return new FrameBuffer{gpu.get(), framebuffer_id};
      }

      uint32_t get() const { return mHandle.get(); }
    };

    bool set_mode(
      Descriptor const &gpu, FrameBuffer const &framebuffer
    , uint32_t connector_id, uint32_t crtc_id, drmModeModeInfo &mode
    ) {
      int error = drmModeSetCrtc(
        gpu.get(), crtc_id, framebuffer.get(), 0, 0, &connector_id, 1, &mode
      );
      if (error) {
        perror("Failed to set mode");
        return false;
      } else {
        return true;
      }
    }

    struct FlipData {
      std::atomic<uint32_t> crtc_id;
      bool flip_is_pending;
    };

    // Holds a reference to page_flip_pending
    bool begin_page_flip(
      Descriptor const &gpu, FrameBuffer const &framebuffer
    , uint32_t crtc_id, FlipData &flip_data
    ) {
      int error = drmModePageFlip(
        gpu.get(), crtc_id, framebuffer.get()
      , DRM_MODE_PAGE_FLIP_EVENT, &flip_data
      );
      if (error) {
        perror("Page flip failed");
        return false;
      } else {
        flip_data.flip_is_pending = true;
        return true;
      }
    }

    namespace detail {
      void mark_flip_no_longer_pending(
        int /*gpu descriptor*/
      , unsigned int /*frame*/
      , unsigned int /*seconds*/
      , unsigned int /*microseconds*/
      , unsigned int crtc_id
      , void *user_data
      ) {
        auto flip_data = static_cast<FlipData *>(user_data);
        assert(flip_data != nullptr);
        if (crtc_id == flip_data->crtc_id) {
          flip_data->flip_is_pending = false;
        }
      }
      drmEventContext make_event_context() {
        drmEventContext context;
        context.version = 3;
        context.page_flip_handler2 = &mark_flip_no_longer_pending;
        return context;
      }
    }

    bool handle_event(Descriptor const &gpu) {
      static drmEventContext context = detail::make_event_context();
      return drmHandleEvent(gpu.get(), &context);
    }
  }

  namespace gbm {
    class Device final {
    private:
      // I'm not sure what guarantees we have -- better safe than sorry
      static void safe_delete(gbm_device *device) {
        if (device != nullptr) gbm_device_destroy(device);
      }
      std::unique_ptr<gbm_device, decltype(&safe_delete)> mHandle;
    public:
      Device() : mHandle{nullptr, &safe_delete} {}
      Device(drm::Descriptor &gpu)
        : mHandle{gbm_create_device(gpu.get()), &safe_delete}
      { if (!mHandle) std::cerr << "Failed to create GBM device" << std::endl; }

      explicit operator bool() const { return mHandle != nullptr; }

      gbm_device *get() const {
        assert(*this);
        return mHandle.get();
      }
    };

    class FrontBuffer final {
    private:
      class Handle {
      private:
        gbm_surface *mSurface;
        gbm_bo *mBuffer;
      public:
        gbm_bo *get() const { return mBuffer; }

        Handle(gbm_surface *surface, gbm_bo *buffer)
          : mSurface{surface}, mBuffer{buffer}
        {}
        Handle() : Handle{nullptr, nullptr} {}
        Handle(Handle const &) = delete;
        Handle &operator=(Handle const &) = delete;
        Handle(Handle &&) = default;
        Handle &operator=(Handle &&) = default;
        ~Handle() {
          gbm_surface_release_buffer(mSurface, mBuffer);
        }

        explicit operator bool() const {
          return mSurface != nullptr && mBuffer != nullptr;
        }
      };
      Handle mHandle;

      FrontBuffer(gbm_surface &surface, gbm_bo &buffer)
        : mHandle{&surface, &buffer}
      {}
    public:
      FrontBuffer() = default;

      explicit operator bool() const { return static_cast<bool>(mHandle); }

      static FrontBuffer create(gbm_surface &surface) {
        gbm_bo *buffer = gbm_surface_lock_front_buffer(&surface);
        if (buffer == nullptr) {
          std::cerr << "Failed to lock front buffer!" << std::endl;
          return FrontBuffer{};
        }
        return FrontBuffer{surface, *buffer};
      }

      // The C API keeps ownership of the underlying buffer objects, and of
      // course there is no hook for buffer creation, so we end up attaching
      // framebuffers to buffer objects on the fly. This returns nullptr on
      // error.
      drm::FrameBuffer *ensure_framebuffer(drm::Descriptor const &gpu) {
        assert(*this);

        auto framebuffer = static_cast<drm::FrameBuffer *>(
          gbm_bo_get_user_data(mHandle.get())
        );
        if (framebuffer != nullptr) return framebuffer;
        framebuffer = drm::FrameBuffer::create(
          gpu
        , gbm_bo_get_width(mHandle.get())
        , gbm_bo_get_height(mHandle.get())
        , gbm_bo_get_stride(mHandle.get())
        , gbm_bo_get_handle(mHandle.get()).u32
        );
        if (framebuffer == nullptr) return nullptr;
        gbm_bo_set_user_data(
          mHandle.get(), framebuffer
        , // This is the deleter
          [](gbm_bo *, void *framebuffer) {
            if (framebuffer == nullptr) return;
            delete static_cast<drm::FrameBuffer *>(framebuffer);
          }
        );
        return framebuffer;
      }
    };

    // This abstracts a swapchain.
    class Surface final {
    private:
      // I'm not sure what guarantees we have -- better safe than sorry
      static void safe_delete(gbm_surface *surface) {
        if (surface != nullptr) gbm_surface_destroy(surface);
      }
      std::unique_ptr<gbm_surface, decltype(&safe_delete)> mHandle;
    public:
      Surface(Device const &device, uint32_t width, uint32_t height)
        : mHandle{
            // Note that gbm_surface_create_with_modifiers also exists
            gbm_surface_create(
              device.get(), width, height
            , // No transparency - 8-bit red, green, blue
              GBM_FORMAT_XRGB8888
            , // Buffer will be presented to the screen
              GBM_BO_USE_SCANOUT |
              // Buffer is to be used for rendering
              GBM_BO_USE_RENDERING
            )
          , &safe_delete
          }
      {
        if (!mHandle) std::cerr << "Failed to create GBM surface" << std::endl;
      }
      Surface() : mHandle{nullptr, &safe_delete} {}

      explicit operator bool() const { return mHandle != nullptr; }

      gbm_surface *get() const {
        assert(*this);
        return mHandle.get();
      }

      FrontBuffer lock_front_buffer() {
        assert(*this);
        return FrontBuffer::create(*mHandle);
      }
    };
  }

  namespace egl {
    class Display final {
    private:
      EGLDisplay mDisplay;
      Display(EGLDisplay display) : mDisplay{display} {}
    public:
      Display() : mDisplay{EGL_NO_DISPLAY} {}
      Display(Display const &) = delete;
      Display &operator=(Display const &) = delete;
      Display(Display &&other) noexcept : mDisplay{other.mDisplay} {
        other.mDisplay = EGL_NO_DISPLAY;
      }
      Display &operator=(Display &&other) noexcept {
        // This class is final, and nothing here can throw exceptions.
        if (this == &other) return *this;
        this->~Display();
        new (this) Display{std::move(other)};
        return *this;
      }
      ~Display() {
        if (mDisplay != EGL_NO_DISPLAY) eglTerminate(mDisplay);
      }

      explicit operator bool() const { return mDisplay != EGL_NO_DISPLAY; }

      EGLDisplay get() const {
        assert(*this);
        return mDisplay;
      }

      static Display create(gbm::Device const &gbm) {
        auto get_platform_display = reinterpret_cast<
          PFNEGLGETPLATFORMDISPLAYEXTPROC
        >(
          eglGetProcAddress("eglGetPlatformDisplayEXT")
        );
        if (get_platform_display == nullptr) {
          std::cerr << "Couldn't find eglGetPlatformDisplay" << std::endl;
          return {};
        }
        EGLDisplay display = get_platform_display(
          EGL_PLATFORM_GBM_KHR, gbm.get(), nullptr
        );
        if (display == EGL_NO_DISPLAY) {
          std::cerr << "Couldn't find EGL display" << std::endl;
          return {};
        }

        EGLint major, minor;
        EGLBoolean success = eglInitialize(display, &major, &minor); 
        if (!success) {
          std::cerr << "Couldn't initialize EGL" << std::endl;
          return {};
        }

        std::cout << "EGL Version: " << eglQueryString(display, EGL_VERSION)
          << std::endl
        ;
        std::cout << "EGL Vendor: " << eglQueryString(display, EGL_VENDOR)
          << std::endl
        ;
        std::cout << "EGL Extensions: "
          << eglQueryString(display, EGL_EXTENSIONS) << std::endl
        ;

        return {display};
      }
    };

    EGLConfig find_config(Display const &display) {
      static constexpr EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT
      , EGL_RED_SIZE, 1
      , EGL_GREEN_SIZE, 1
      , EGL_BLUE_SIZE, 1
      , EGL_ALPHA_SIZE, 0
      , EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT
      , EGL_NONE
      };

      EGLConfig config;
      EGLint num_processed;
      EGLBoolean success = eglChooseConfig(
        display.get(), config_attributes, &config, 1, &num_processed
      );
      if (!success || num_processed != 1 || config == nullptr) {
        std::cerr << "eglChooseConfig failed" << std::endl;
        return nullptr;
      }
      return config;
    }

    class Context final {
    private:
      EGLDisplay mDisplay;
      EGLContext mContext;
      EGLConfig mConfig;
      Context(EGLDisplay display, EGLContext context, EGLConfig config)
        : mDisplay{display}, mContext{context}, mConfig{config}
      {}
    public:
      Context() : mDisplay{}, mContext{EGL_NO_CONTEXT}, mConfig{nullptr} {}
      Context(Context const &) = delete;
      Context &operator=(Context const &) = delete;
      Context(Context &&other) noexcept
        : mDisplay{other.mDisplay}
        , mContext{other.mContext}
      {
        other.mDisplay = EGL_NO_DISPLAY;
        other.mContext = EGL_NO_CONTEXT;
      }
      Context &operator=(Context &&other) noexcept {
        // This class is final, and nothing here can throw exceptions.
        if (this == &other) return *this;
        this->~Context();
        new (this) Context{std::move(other)};
        return *this;
      }
      ~Context() {
        if (mContext != EGL_NO_CONTEXT) eglDestroyContext(mDisplay, mContext);
      }

      explicit operator bool() const {
        return mDisplay != EGL_NO_DISPLAY && mContext != EGL_NO_CONTEXT
            && mConfig != nullptr
        ;
      }

      EGLContext get() const { assert(*this); return mContext; }

      // Keeps a reference to the display!
      // This function creates global, thread-local state! See ThreadContext.
      static Context create(
        Display const &display, EGLConfig config
      , Context const *shared_context = nullptr
      ) {
        assert(config != nullptr);

        EGLBoolean success = eglBindAPI(EGL_OPENGL_ES_API);
        if (!success) {
          std::cerr << "Couldn't use OpenGL ES 3" << std::endl;
          return {};
        }

        static constexpr EGLint attributes[] = {
          EGL_CONTEXT_CLIENT_VERSION, 3
        , EGL_NONE
        };
        EGLContext share = EGL_NO_CONTEXT;
        if (shared_context != nullptr) {
          // This shouldn't be nullptr, but it won't break if it is
          share = shared_context->mContext;
        }
        EGLContext context = eglCreateContext(
          display.get(), config, share, attributes
        );
        if (context == EGL_NO_CONTEXT) {
          std::cerr << "Failed to create OpenGL context" << std::endl;
          return {};
        }

        return {display.get(), context,config};
      }
    };

    class Surface final {
    private:
      EGLDisplay mDisplay;
      EGLSurface mSurface;
      Surface(EGLDisplay display, EGLSurface surface)
        : mDisplay{display}, mSurface{surface}
      {}
    public:
      Surface() : mDisplay{EGL_NO_DISPLAY}, mSurface{EGL_NO_SURFACE} {}
      Surface(Surface const &) = delete;
      Surface &operator=(Surface const &) = delete;
      Surface(Surface &&other)
        : mDisplay{other.mDisplay}, mSurface{other.mSurface}
      { other.mDisplay = EGL_NO_DISPLAY; other.mSurface = EGL_NO_SURFACE; }
      Surface &operator=(Surface &&other) {
        // This class is final, and nothing here can throw exceptions.
        if (this == &other) return *this;
        this->~Surface();
        new (this) Surface{std::move(other)};
        return *this;
      }
      ~Surface() { if (*this) eglDestroySurface(mDisplay, mSurface); }

      explicit operator bool() const {
        return mDisplay != EGL_NO_DISPLAY && mSurface != EGL_NO_SURFACE;
      }

      EGLSurface get() const { assert(*this); return mSurface; }

      static Surface create(
        Display const &display, EGLConfig config
      , gbm::Surface const &gbm_surface
      ) {
        assert(config != nullptr);
        EGLSurface surface = eglCreateWindowSurface(
          display.get(), config, gbm_surface.get(), nullptr
        );
        if (surface == EGL_NO_SURFACE) {
          std::cerr << "Failed to create EGL surface" << std::endl;
          return {};
        } else {
          return {display.get(), surface};
        }
      }
    };

    class BoundContext final {
    private:
      EGLDisplay mDisplay;
      BoundContext(EGLDisplay display) : mDisplay{display} {}
    public:
      BoundContext() : mDisplay{EGL_NO_DISPLAY} {}
      BoundContext(BoundContext const &) = delete;
      BoundContext &operator=(BoundContext const &) = delete;
      BoundContext(BoundContext &&other) noexcept
        : mDisplay{other.mDisplay}
      { other.mDisplay = EGL_NO_DISPLAY; }
      BoundContext &operator=(BoundContext &&other) noexcept {
        // This class is final, and nothing here can throw exceptions.
        if (this == &other) return *this;
        this->~BoundContext();
        new (this) BoundContext{std::move(other)};
        return *this;
      }
      ~BoundContext() {
        if (*this) eglMakeCurrent(
          mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT
        );
      }

      explicit operator bool() const { return mDisplay != nullptr; }

      // This function creates global, thread-local state! See ThreadContext.
      static BoundContext create(
        Display const &display, Surface const &surface, Context const &context
      ) {
        EGLBoolean success = eglMakeCurrent(
          display.get(), surface.get(), surface.get(), context.get()
        );
        if (!success) {
          std::cerr << "Failed to make context current" << std::endl;
          return {};
        }
        return {display.get()};
      }

      static BoundContext create(
        Display const &display, Context const &context
      ) {
        // Something is wrong if this thread already has a context bound
        assert(eglGetCurrentContext() == EGL_NO_CONTEXT);

        EGLBoolean success = eglMakeCurrent(
          display.get(), EGL_NO_SURFACE, EGL_NO_SURFACE, context.get()
        );
        if (!success) {
          std::cerr << "Failed to make context current" << std::endl;
          return {};
        }
        return {display.get()};
      }
    };

    class DrawableContext {
    private:
      Context mContext;
      Surface mSurface;
      BoundContext mBoundContext;

      DrawableContext(
        Context context
      , Surface surface
      , BoundContext bound
      ) : mContext{std::move(context)}
        , mSurface{std::move(surface)}
        , mBoundContext{std::move(bound)}
      {}

    public:
      DrawableContext() = default;

      static DrawableContext create(
        Display const &display
      , gbm::Surface const &gbm_surface
      , Context const *shared = nullptr
      ) {
        EGLConfig config = find_config(display);
        if (!config) return {};

        auto context = Context::create(display, config, shared);
        if (!context) return {};

        auto surface = Surface::create(display, config, gbm_surface);
        if (!surface) return {};

        auto bound = BoundContext::create(display, surface, context);
        if (!bound) return {};

        return {std::move(context), std::move(surface), std::move(bound)};
      }

      explicit operator bool() const { return static_cast<bool>(mContext); }

      void swap_buffers(Display const &display) {
        assert(*this);
        eglSwapBuffers(display.get(), mSurface.get());
      }
    };

    class SurfacelessContext {
    private:
      Context mContext;
      BoundContext mBoundContext;

      SurfacelessContext(
        Context context
      , BoundContext bound
      ) : mContext{std::move(context)}
        , mBoundContext{std::move(bound)}
      {}

    public:
      SurfacelessContext() = default;

      static SurfacelessContext create(Display const &display) {
        EGLConfig config = find_config(display);
        if (!config) return {};

        auto context = Context::create(display, config);
        if (!context) return {};

        auto bound = BoundContext::create(display, context);
        if (!bound) return {};

        return {std::move(context), std::move(bound)};
      }

      explicit operator bool() const { return static_cast<bool>(mContext); }

      // Call this on another thread!
      DrawableContext create_child_context(
        Display const &display, gbm::Surface const &gbm_surface
      ) const {
        assert(*this);
        return DrawableContext::create(display, gbm_surface, &mContext);
      }
    };
  }

  // Instances of this class contain implicit global, thread-local state due
  // to the nature of the EGL/OpenGL APIs. It should not be moved across
  // thread boundaries.
  class ActiveDisplay {
  private:
    std::thread::id mThreadID;
    gbm::Surface mSurface;
    egl::DrawableContext mEGL;
    drm::FlipData mFlipData;
    gbm::FrontBuffer mCurrentFrontBuffer;
    gbm::FrontBuffer mNextFrontBuffer;

  public:
    ActiveDisplay(
      gbm::Surface gbm_surface
    , egl::DrawableContext context
    , uint32_t crtc_id
    ) : mThreadID{std::this_thread::get_id()}
      , mSurface{std::move(gbm_surface)}
      , mEGL{std::move(context)}
      , mFlipData{crtc_id, false}
      , mCurrentFrontBuffer{}, mNextFrontBuffer{}
    {}

    static std::optional<ActiveDisplay> create(
      gbm::Device const &gbm, egl::Display const &egl
    , uint32_t width, uint32_t height
    , uint32_t crtc_id // TODO - needed?
    ) {
      gbm::Surface gbm_surface{gbm, width, height};
      if (!gbm_surface) return std::nullopt;

      auto context = egl::DrawableContext::create(egl, gbm_surface);
      if (!context) return std::nullopt;

      return std::make_optional<ActiveDisplay>(
        std::move(gbm_surface), std::move(context), crtc_id
      );
    }

    explicit operator bool() const {
      // Prevent using this on a thread other than the one it was created on
      return (std::this_thread::get_id() == mThreadID) && mSurface && mEGL;
    }

    uint32_t crtc_id() const { assert(*this); return mFlipData.crtc_id; }

    bool set_mode(
      drm::Descriptor const &gpu
    , egl::Display const &egl_display
    , uint32_t connector_id
    , drmModeModeInfo &mode
    ) {
      assert(*this);
      glClearColor(0.5, 0.5, 0.5, 1.0);
      glClear(GL_COLOR_BUFFER_BIT);
      mEGL.swap_buffers(egl_display);
      auto front = mSurface.lock_front_buffer();
      if (!front) return false;
      auto framebuffer = front.ensure_framebuffer(gpu);
      if (!framebuffer) return false;
      if (drm::set_mode(
        gpu, *framebuffer, connector_id, mFlipData.crtc_id, mode
      )) {
        mCurrentFrontBuffer = std::move(front);
        return true;
      } else {
        return false;
      }
    }

    bool begin_swap_buffers(
      drm::Descriptor const &gpu
    , egl::Display const &egl_display
    ) {
      assert(*this && mCurrentFrontBuffer);
      mEGL.swap_buffers(egl_display);
      auto front = mSurface.lock_front_buffer();
      if (!front) return false;
      auto framebuffer = front.ensure_framebuffer(gpu);
      if (!framebuffer) return false;
      bool success = drm::begin_page_flip(
        gpu, *framebuffer, mFlipData.crtc_id, mFlipData
      );
      if (success) mNextFrontBuffer = std::move(front);
      return success;
    }

    bool buffer_swap_is_pending() const {
      assert(*this);
      return mFlipData.flip_is_pending;
    }

    bool handle_event(drm::Descriptor const &gpu) {
      assert(*this && mFlipData.flip_is_pending);
      return drm::handle_event(gpu);
    }

    void finish_swap_buffers() {
      assert(*this && !mFlipData.flip_is_pending);
      mCurrentFrontBuffer = std::move(mNextFrontBuffer);
    }
  };

  namespace asio = boost::asio;

  template <typename DrawCallback>
  class DrawRoutine final {
  private:
    enum class State { MODE_SET, DRAWING, PAGE_FLIP, STOPPED };
    std::atomic<bool> const &mKeepRunning;
    asio::io_service &mASIO;
    drm::Descriptor const&mDRM;
    uint32_t mConnectorID;
    drmModeModeInfo &mMode;
    egl::Display const &mEGL;
    asio::posix::stream_descriptor mDescriptor;
    std::optional<ActiveDisplay> mDisplay;
    DrawCallback mDrawCallback;
    State mState;

    DrawRoutine(
      std::atomic<bool> const &keep_running
    , asio::io_service &asio
    , drm::Descriptor const &drm
    , gbm::Device const &gbm
    , egl::Display const &egl
    , uint32_t connector_id
    , uint32_t crtc_id
    , drmModeModeInfo &mode
    , DrawCallback draw_callback
    ) : mKeepRunning{keep_running}
      , mASIO{asio}
      , mDRM{drm}
      , mConnectorID{connector_id}
      , mMode{mode}
      , mEGL{egl}
      , mDescriptor{asio, drm.get()}
      , mDisplay{ActiveDisplay::create(
          gbm, egl, mode.hdisplay, mode.vdisplay, crtc_id
        )}
      , mDrawCallback{std::move(draw_callback)}
      , mState{State::MODE_SET}
    {}

    class Worker final {
    private:
      DrawRoutine *self;
    public:
      Worker(DrawRoutine &state) : self{&state} {}

      // Declare but don't define copy semantics. We're using the boost copy of
      // asio for now for convenience, but it doesn't want to accept move-only
      // handlers. This trick outsmarts its checks (see
      // https://stackoverflow.com/questions/17211263/how-to-trick-boostasio-to-allow-move-only-handlers)
      Worker(Worker const &);
      Worker &operator=(Worker const &);
      Worker(Worker &&other) noexcept : self{other.self} {
        other.self = nullptr;
      }
      Worker &operator=(Worker &&other) {
        self = other.self;
        other.self = nullptr;
      }
      ~Worker() = default;

      explicit operator bool() const { return self != nullptr; }
      void operator()(boost::system::error_code error = {}, std::size_t = 0) {
        assert(*self);

        if (error) {
          std::cerr << "ASIO error: " << error.message() << std::endl;
          std::cerr << "Thread exiting due to error" << std::endl;
          return;
        }

        if (!self->mKeepRunning) {
          std::cout << "Thread exiting due to external request" << std::endl;
          self->mState = State::STOPPED;
          return;
        }

        switch (self->mState) {
        case State::STOPPED:
          assert(false);
        case State::MODE_SET:
          if (!self->mDisplay->set_mode(
            self->mDRM, self->mEGL, self->mConnectorID, self->mMode
          )) {
            std::cerr << "Thread exiting due to error" << std::endl;
            return;
          }
          self->mState = State::DRAWING;
          self->mASIO.post(std::move(*this));
          return;
        case State::DRAWING:
          // Do the drawing
          self->mDrawCallback();

          // Begin the flip
          if (!self->mDisplay->begin_swap_buffers(self->mDRM, self->mEGL)) {
            std::cerr << "Thread exiting due to error" << std::endl;
            return;
          }
          // Wait for the flip
          self->mState = State::PAGE_FLIP;
          asio::async_read(
            self->mDescriptor, asio::null_buffers(), std::move(*this)
          );
          return;
        case State::PAGE_FLIP:
          self->mDisplay->handle_event(self->mDRM);
          if (self->mDisplay->buffer_swap_is_pending()) {
            // Keep waiting
            asio::async_read(
              self->mDescriptor, asio::null_buffers(), std::move(*this)
            );
            return;
          } else {
            // Complete the flip
            self->mDisplay->finish_swap_buffers();

            // Draw the next frame
            self->mState = State::DRAWING;
            self->mASIO.post(std::move(*this));
            return;
          }
        }
      }
    };

  public:
    explicit operator bool() const { return static_cast<bool>(mDisplay); }

    static bool begin(
      std::atomic<bool> const &keep_running
    , asio::io_service &asio
    , drm::Descriptor const &drm
    , gbm::Device const &gbm
    , egl::Display const &egl
    , uint32_t connector_id
    , uint32_t crtc_id
    , drmModeModeInfo &mode
    , DrawCallback draw_callback
    ) {
      auto display = ActiveDisplay::create(
        gbm, egl, mode.hdisplay, mode.vdisplay, crtc_id
      );
      DrawRoutine state{
        keep_running, asio, drm, gbm, egl, connector_id, crtc_id, mode
      , std::move(draw_callback)
      };
      if (!state) return false;
      Worker{state}();
      return true;
    }
  };

  class RAIIThread {
  private:
    std::thread mThread;
  public:
    RAIIThread() = default;
    template <typename ...Args>
    RAIIThread(Args&&... args) : mThread{std::forward<Args>(args)...} {}
    RAIIThread(RAIIThread const &) = delete;
    RAIIThread &operator=(RAIIThread const &) = delete;
    RAIIThread(RAIIThread &&) = default;
    RAIIThread &operator=(RAIIThread &&) = default;
    ~RAIIThread() { if (mThread.joinable()) mThread.join(); }

    explicit operator bool() const { return mThread.joinable(); }
  };

  class DrawThread final {
  private:
    std::atomic<bool> mKeepRunning;
    asio::io_service mASIO;
    uint32_t mCrtcID;
    RAIIThread mThread;
  public:
    // Run something on the drawing thread
    template <typename Callback>
    void post(Callback &&callback) {
      mASIO.post(std::forward<Callback>(callback));
    }

    uint32_t crtc_id() const { return mCrtcID; }

    void stop() { mKeepRunning = false; }

    explicit operator bool() const { return static_cast<bool>(mThread); }

    template <typename DrawCallback>
    DrawThread(
      drm::Descriptor const &drm
    , gbm::Device const &gbm
    , egl::Display const &egl
    , uint32_t connector_id
    , uint32_t crtc_id
    , drmModeModeInfo &mode
    , DrawCallback draw_callback
    ) : mKeepRunning{false}
      , mASIO{}
      , mCrtcID{crtc_id}
      , mThread{[
          this, &drm, &gbm, &egl
        , connector_id, crtc_id, &mode
        , draw_callback = std::move(draw_callback)
        ]() {
          if (DrawRoutine<DrawCallback>::begin(
            mKeepRunning
          , mASIO
          , drm, gbm, egl
          , connector_id, crtc_id, mode
          , std::move(draw_callback)
          )) {
            mASIO.run();
          }
        }}
    {}
  };

  class DeviceManager final {
  private:
    drm::Descriptor mDRM;
    gbm::Device mGBM;
    egl::Display mEGL;
    // The keys here are connector ids returned from libdrm. The hope is that
    // they are consistent across reboots etc.
    std::map<uint32_t, DrawThread> mDisplayLookup;
    std::set<uint32_t> mUnusedCrtcs;

    std::optional<uint32_t> find_crtc_for_connector(
      drm::Resources const &resources, drm::Connector const &connector
    ) {
      for (uint32_t encoder_id : connector.encoders()) {
        drm::Encoder encoder{mDRM, encoder_id};
        if (!encoder) continue;
        int i = 0;
        for (uint32_t crtc_id : resources.crtcs()) {
          bool unused = mUnusedCrtcs.find(crtc_id) != mUnusedCrtcs.end();
          if (encoder.has_crtc(i) && unused) return crtc_id;
          ++i;
        }
      }
      std::cerr << "No crtc found" << std::endl;
      return std::nullopt;
    }

    DeviceManager(
      drm::Descriptor gpu, gbm::Device gbm, egl::Display egl
    , std::set<uint32_t> unused_crtcs
    ) : mDRM{std::move(gpu)}
      , mGBM{std::move(gbm)}
      , mEGL{std::move(egl)}
      , mDisplayLookup{}
      , mUnusedCrtcs{std::move(unused_crtcs)}
    {}

  public:
    DeviceManager() : DeviceManager({}, {}, {}, {}) {}

    static DeviceManager create(char const *path) {
      auto gpu = drm::Descriptor::create(path);
      if (!gpu) return {};

      drm::Resources resources{gpu};
      if (!resources) return {};

      gbm::Device gbm{gpu};
      if (!gbm) return {};

      auto egl = egl::Display::create(gbm);
      if (!egl) return {};

      std::set<uint32_t> unused_crtcs{};
      for (uint32_t crtc_id : resources.crtcs()) unused_crtcs.insert(crtc_id);

      return {
        std::move(gpu), std::move(gbm), std::move(egl), std::move(unused_crtcs)
      };
    }

    explicit operator bool() const {
      return mDRM && mGBM && mEGL;
    }

    void update_connections() {
      assert(*this);

      drm::Resources resources{mDRM};
      if (!resources) return;

      for (uint32_t connector_id : resources.connectors()) {
        drm::Connector connector{mDRM, connector_id};
        if (!connector) continue;

        if (
          auto it = mDisplayLookup.find(connector.id());
          it != mDisplayLookup.end()
        ) {
          if (!connector.is_connected()) {
            // Someone unplugged it!
            mUnusedCrtcs.insert(it->second.crtc_id());
            mDisplayLookup.erase(it);
          }
        } else if (connector.is_connected()) {
          // Someone plugged it in!
          drmModeModeInfo *mode = connector.find_best_mode();
          if (!mode) continue;

          auto crtc_id = find_crtc_for_connector(resources, connector);
          if (!crtc_id) continue;

          std::cout << "Found display " << *crtc_id
                    << " at " << mode->hdisplay << "x" << mode->vdisplay
                    << std::endl
          ;

          mDisplayLookup.emplace(
            std::piecewise_construct
          , std::forward_as_tuple(connector.id())
          , std::forward_as_tuple(
              mDRM, mGBM, mEGL, connector.id(), *crtc_id, *mode
            , []() {
                glClearColor(0.5, 0.5, 0.5, 1.0);
                glClear(GL_COLOR_BUFFER_BIT);
              }
            )
          );
          mUnusedCrtcs.erase(*crtc_id);
        }
      }
    }
  };
}

int main() {
  auto drm = DeviceManager::create("/dev/dri/card0");
  if (!drm) return EXIT_FAILURE;
  drm.update_connections();
  return EXIT_SUCCESS;
}