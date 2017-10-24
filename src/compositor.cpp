#include <iostream>
#include <memory>
#include <optional>

#include <cassert>
#include <cstdlib>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
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

    explicit operator bool() {
      return mHandle >= 0;
    }

    int get() {
      assert(*this);
      return mHandle;
    }
  };

  class DirectRenderingManager {
  private:
    FileDescriptor mDescriptor;
    drmModeModeInfo &mModeInfo;
    uint32_t mCRTControllerID;
    uint32_t mConnectorID;

    class Encoder {
    private:
      std::unique_ptr<drmModeEncoder, decltype(&drmModeFreeEncoder)> mHandle;
    public:
      Encoder() : mHandle{nullptr, &drmModeFreeEncoder} {}
      Encoder(int file_descriptor, uint32_t encoder_id)
        : mHandle{
            drmModeGetEncoder(file_descriptor, encoder_id)
          , &drmModeFreeEncoder
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
      std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>
        mHandle
      ;
    public:
      Connector() : mHandle{nullptr, &drmModeFreeConnector} {}
      Connector(int file_descriptor, uint32_t connector_id)
        : mHandle{
            drmModeGetConnector(file_descriptor, connector_id)
          , &drmModeFreeConnector
          }
      { if (!mHandle) perror("Couldn't get connector"); }

      explicit operator bool() const { return mHandle != nullptr; }

      bool is_connected() const {
        return mHandle->connection == DRM_MODE_CONNECTED;
      }

      uint32_t id() const { return mHandle->connector_id; }

      uint32_t encoder_id() const {
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

      template <typename Callback>
      void each_encoder(int file_descriptor, Callback &&callback) const {
        assert(*this);
        for (int i = 0; i < mHandle->count_encoders; i++) {
          auto encoder = Encoder{file_descriptor, mHandle->encoders[i]};
          if (encoder) if (callback(std::move(encoder))) return;
        }
      }
    };

    class Resources {
    private:
      std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> mHandle;
    public:
      Resources(int file_descriptor)
        : mHandle{drmModeGetResources(file_descriptor), &drmModeFreeResources}
      { if (!mHandle) perror("Couldn't retrieve DRM resources"); }

      explicit operator bool() const { return mHandle != nullptr; }

      template <typename Callback>
      void each_connector(int file_descriptor, Callback &&callback) const {
        assert(*this);
        for (int i = 0; i < mHandle->count_connectors; i++) {
          auto connector = Connector{file_descriptor, mHandle->connectors[i]};
          if (connector) if (callback(std::move(connector))) return;
        }
      }

      template <typename Callback>
      void each_encoder(int file_descriptor, Callback &&callback) const {
        assert(*this);
        for (int i = 0; i < mHandle->count_encoders; i++) {
          auto encoder = Encoder{file_descriptor, mHandle->encoders[i]};
          if (encoder) if (callback(std::move(encoder))) return;
        }
      }
    };

    static Connector find_connector(int file_descriptor, Resources &resources) {
      // Just choose the first connection for now
      Connector result{};
      resources.each_connector(
        file_descriptor
      , [&](Connector connector) {
          if (connector.is_connected()) {
            result = std::move(connector);
            return true;
          } else return false;
        }
      );
      if (!result) std::cerr << "No connected connector" << std::endl;
      return result;
    }

    static auto find_encoder(
      int file_descriptor
    , Resources const &resources
    , Connector const &connector
    ) {
      Encoder result{};
      resources.each_encoder(
        file_descriptor
      , [&](Encoder encoder) {
          if (encoder.id() == connector.encoder_id()) {
            result = std::move(encoder);
            return true;
          } else return false;
        }
      );
      if (!result) std::cerr << "No encoder found" << std::endl;
      return result;
    }

    DirectRenderingManager(
      FileDescriptor descriptor, drmModeModeInfo &mode_info
    , uint32_t crt_controller_id, uint32_t connector_id
    ) : mDescriptor{std::move(descriptor)}, mModeInfo{mode_info}
      , mCRTControllerID{crt_controller_id}, mConnectorID{connector_id}
    {}

  public:
    static std::optional<DirectRenderingManager> create(char const *path) {
      FileDescriptor descriptor{path};
      if (!descriptor) return std::nullopt;

      Resources resources{descriptor.get()};
      if (!resources) return std::nullopt;

      Connector connector = find_connector(descriptor.get(), resources);
      if (!connector) return std::nullopt;

      auto mode = connector.find_best_mode();
      if (!mode) return std::nullopt;

      auto encoder = find_encoder(descriptor.get(), resources, connector);
      // TODO - fancier stuff here
      if (!encoder) return std::nullopt;

      return DirectRenderingManager{
        std::move(descriptor), *mode, encoder.crtc_id(), connector.id()
      };
    }
  };

  //class GraphicsBufferManager {
  //};
}

int main() {
  DirectRenderingManager::create("/dev/dri/card0");
  return EXIT_SUCCESS;
}