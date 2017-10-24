#include <iostream>
#include <memory>
#include <optional>
#include <unordered_map>

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

    explicit operator bool() const {
      return mHandle >= 0;
    }

    int get() const {
      assert(*this);
      return mHandle;
    }
  };

  class DirectRenderingManager {
  private:
    //struct Connection {
    //  drmModeModeInfo &mModeInfo;
    //  uint32_t mCRTControllerID;
    //  uint32_t mConnectorID;
    //};

    FileDescriptor mDescriptor;
    std::unordered_map<uint32_t, uint32_t> mConnectorCrtcLookup;

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

  public:
    DirectRenderingManager(
      FileDescriptor descriptor
    ) : mDescriptor{std::move(descriptor)}, mConnectorCrtcLookup{}
    {}

    explicit operator bool() const { return static_cast<bool>(mDescriptor); }

    void update_connections() {
      assert (*this);

      Resources resources{mDescriptor.get()};
      if (!resources) return;

      std::unordered_map<uint32_t, uint32_t> new_lookup{};
      resources.each_connector(
        mDescriptor.get()
      , [&](Connector connector) {
          auto it = mConnectorCrtcLookup.find(connector.id());
          if (it != mConnectorCrtcLookup.end()) {
            auto node = mConnectorCrtcLookup.extract(it);
            if (connector.is_connected()) {
              new_lookup.insert(std::move(node));
            }
          } else if (connector.is_connected()) {
            auto mode = connector.find_best_mode();
            if (!mode) return false;

            auto encoder = find_encoder(
              mDescriptor.get(), resources, connector
            );
            // TODO - fancier stuff here
            if (!encoder) return false;
            new_lookup.emplace(connector.id(), encoder.crtc_id());
          }
          return false;
        }
      );
      mConnectorCrtcLookup = std::move(new_lookup);
    }
  };

  //class GraphicsBufferManager {
  //};
}

int main() {
  DirectRenderingManager drm{"/dev/dri/card0"};
  drm.update_connections();
  return EXIT_SUCCESS;
}