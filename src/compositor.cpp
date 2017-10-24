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
    using Resources = std::unique_ptr<
      drmModeRes, decltype(&drmModeFreeResources)
    >;

    using Connector = std::unique_ptr<
      drmModeConnector, decltype(&drmModeFreeConnector)
    >;

    using Encoder = std::unique_ptr<
      drmModeEncoder, decltype(&drmModeFreeEncoder)
    >;

    FileDescriptor mDescriptor;
    drmModeModeInfo &mModeInfo;
    uint32_t mCRTControllerID;
    uint32_t mConnectorID;

    static auto
    drmResources(int file_descriptor) {
      auto result = Resources {
        drmModeGetResources(file_descriptor), &drmModeFreeResources
      };
      if (!result) perror("Couldn't retrieve DRM resources");
      return result;
    }

    static auto
    drmConnector(int file_descriptor, uint32_t connector_id) {
      auto result = Connector {
        drmModeGetConnector(file_descriptor, connector_id)
      , &drmModeFreeConnector
      };
      if (!result) perror("Couldn't get connector");
      return result;
    }

    static auto
    drmEncoder(int file_descriptor, uint32_t encoder_id) {
      auto result = Encoder {
        drmModeGetEncoder(file_descriptor, encoder_id)
      , &drmModeFreeEncoder
      };
      if (!result) perror("Couldn't get encoder");
      return result;
    }

    static Connector findConnector(int file_descriptor, drmModeRes *resources) {
      // Just choose the first connection for now
      for (int i = 0; i < resources->count_connectors; i++) {
        auto connector = drmConnector(
          file_descriptor, resources->connectors[i]
        );
        if (connector->connection == DRM_MODE_CONNECTED) {
          return connector;
        }
      }
      std::cerr << "No connected connector" << std::endl;
      return Connector {nullptr, &drmModeFreeConnector};
    }

    static drmModeModeInfo *findMode(drmModeConnector *connector) {
      drmModeModeInfo *result = nullptr;
      for (int i = 0, biggest_area = 0; i < connector->count_modes; i++) {
        drmModeModeInfo &mode = connector->modes[i];
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

    static auto findEncoder(
      int file_descriptor, drmModeRes *resources, drmModeConnector *connector
    ) {
      for (int i = 0; i < resources->count_encoders; i++) {
        auto encoder = drmEncoder(file_descriptor, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id) {
          return encoder;
        }
      }
      std::cerr << "No encoder found" << std::endl;
      return Encoder {nullptr, &drmModeFreeEncoder};
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

      auto resources = drmResources(descriptor.get());
      if (!resources) return std::nullopt;

      auto connector = findConnector(descriptor.get(), resources.get());
      if (!connector) return std::nullopt;

      auto mode = findMode(connector.get());
      if (!mode) return std::nullopt;

      auto encoder = findEncoder(
        descriptor.get(), resources.get(), connector.get()
      );
      // TODO - fancier stuff here
      if (!encoder) return std::nullopt;

      return DirectRenderingManager{
        std::move(descriptor), *mode, encoder->crtc_id, connector->connector_id
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