#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

#include <cassert>
#include <cstdlib>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace {
  template <typename Arg>
  void perror(Arg&& message) {
    static constexpr std::size_t errno_buffer_size = 256;
    char buffer[errno_buffer_size];
    strerror_r(errno, buffer, errno_buffer_size);
    std::cerr << message << ": " << buffer << std::endl;
  }

  class FileDescriptor {
  private:
    int mHandle;
  public:
    FileDescriptor(char const *path)
      : mHandle{open(path, O_RDWR)}
    {
      if (!mHandle) perror("Couldn't open file");
    }

    FileDescriptor(FileDescriptor const &) = delete;
    FileDescriptor(FileDescriptor &&other)
      : mHandle{other.mHandle}
    {
      other.mHandle = -1;
    }
    FileDescriptor &operator=(FileDescriptor const &) = delete;
    FileDescriptor &operator=(FileDescriptor &&other) {
      mHandle = other.mHandle;
      other.mHandle = -1;
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
  class Span {
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
    class Encoder {
    private:
      // I'm not sure what guarantees we have -- better safe than sorry
      static void safe_delete(drmModeEncoder *encoder) {
        if (encoder != nullptr) drmModeFreeEncoder(encoder);
      }
      std::unique_ptr<drmModeEncoder, decltype(&safe_delete)> mHandle;
    public:
      Encoder() : mHandle{nullptr, &safe_delete} {}
      Encoder(FileDescriptor const &file_descriptor, uint32_t encoder_id)
        : mHandle{
            drmModeGetEncoder(file_descriptor.get(), encoder_id)
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

    class Connector {
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
      Connector(FileDescriptor const &file_descriptor, uint32_t connector_id)
        : mHandle{
            drmModeGetConnector(file_descriptor.get(), connector_id)
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

    class Resources {
    private:
      // I'm not sure what guarantees we have -- better safe than sorry
      static void safe_delete(drmModeRes *resources) {
        if (resources != nullptr) drmModeFreeResources(resources);
      }
      std::unique_ptr<drmModeRes, decltype(&safe_delete)> mHandle;

    public:
      Resources(FileDescriptor const &file_descriptor)
        : mHandle{drmModeGetResources(file_descriptor.get()), &safe_delete}
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
    };
  }

  namespace gbm {
    class Device {
    private:
      // I'm not sure what guarantees we have -- better safe than sorry
      static void safe_delete(struct gbm_device *device) {
        if (device != nullptr) gbm_device_destroy(device);
      }
      std::unique_ptr<struct gbm_device, decltype(&safe_delete)> mHandle;
    public:
      Device() : mHandle{nullptr, &safe_delete} {}
      Device(FileDescriptor &descriptor)
        : mHandle{gbm_create_device(descriptor.get()), &safe_delete}
      { if (!mHandle) std::cerr << "Failed to create GBM device" << std::endl; }

      explicit operator bool() const { return mHandle != nullptr; }

      struct gbm_device *get() const {
        assert(*this);
        return mHandle.get();
      }
      //struct gbm_device &operator*() { return *mHandle; }
      //struct gbm_device const &operator*() const { return *mHandle; }
    };

    class Surface {
    private:
      // I'm not sure what guarantees we have -- better safe than sorry
      static void safe_delete(struct gbm_surface *surface) {
        if (surface != nullptr) gbm_surface_destroy(surface);
      }
      std::unique_ptr<struct gbm_surface, decltype(&safe_delete)> mHandle;
    public:
      Surface(Device const &device, uint32_t width, uint32_t height)
        : mHandle{
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

      explicit operator bool() const { return mHandle != nullptr; }

      //struct gbm_surface &operator*() { return *mHandle; }
      //struct gbm_surface const &operator*() const { return *mHandle; }
    };
  }

  class Display {
  private:
    gbm::Surface mSurface;
    uint32_t mCrtcID;
  public:
    Display(gbm::Surface surface, uint32_t crtc_id)
      : mSurface{std::move(surface)}, mCrtcID{crtc_id}
    {}

    uint32_t crtc_id() const { return mCrtcID; }
  };

  class DeviceManager {
  private:
    FileDescriptor mGPUDescriptor;
    gbm::Device mGBM;
    std::unordered_map<uint32_t, Display> mDisplayLookup;
    std::set<uint32_t> mUnusedCrtcs;

    std::optional<uint32_t> find_crtc_for_connector(
      drm::Resources const &resources, drm::Connector const &connector
    ) {
      for (uint32_t encoder_id : connector.encoders()) {
        drm::Encoder encoder{mGPUDescriptor, encoder_id};
        if (!encoder) continue;
        int i = 0;
        for (uint32_t crtc_id : resources.crtcs()) {
          bool unused = mUnusedCrtcs.find(crtc_id) == mUnusedCrtcs.end();
          if (encoder.has_crtc(i) && unused) return crtc_id;
          ++i;
        }
      }
      return std::nullopt;
    }

    DeviceManager(FileDescriptor descriptor, std::set<uint32_t> unused_crtcs)
      : mGPUDescriptor{std::move(descriptor)}
      , mGBM{mGPUDescriptor}
      , mDisplayLookup{}
      , mUnusedCrtcs{std::move(unused_crtcs)}
    {}

  public:
    static std::optional<DeviceManager> create(char const *path) {
      FileDescriptor descriptor{path};
      if (!descriptor) return std::nullopt;

      drm::Resources resources{descriptor};
      if (!resources) return std::nullopt;

      std::set<uint32_t> unused_crtcs{};
      for (uint32_t crtc_id : resources.crtcs()) unused_crtcs.insert(crtc_id);

      return DeviceManager{std::move(descriptor), std::move(unused_crtcs)};
    }

    explicit operator bool() const { return static_cast<bool>(mGPUDescriptor); }

    void update_connections() {
      assert (*this);

      drm::Resources resources{mGPUDescriptor};
      if (!resources) return;

      for (uint32_t connector_id : resources.connectors()) {
        drm::Connector connector{mGPUDescriptor, connector_id};
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

          gbm::Surface surface{mGBM, mode->hdisplay, mode->vdisplay};
          if (!surface) continue;

          mDisplayLookup.emplace(
            std::piecewise_construct
          , std::forward_as_tuple(connector.id())
          , std::forward_as_tuple(std::move(surface), *crtc_id)
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
  drm->update_connections();
  return EXIT_SUCCESS;
}