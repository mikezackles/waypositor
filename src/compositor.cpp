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
    FileDescriptor mDescriptor;
    std::unordered_map<uint32_t, uint32_t> mConnectorCrtcLookup;
    std::set<uint32_t> mUnusedCrtcs;

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

      class Encoders {
      private:
        drmModeConnector const &mHandle;
      public:
        Encoders(drmModeConnector const &handle) : mHandle{handle} {}
        auto begin() const { return mHandle.encoders; }
        auto end() const { return mHandle.encoders + mHandle.count_encoders; }
      };
      Encoders encoders() const { return {*mHandle.get()}; }
    };

    class Resources {
    private:
      std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> mHandle;

    public:
      Resources(int file_descriptor)
        : mHandle{drmModeGetResources(file_descriptor), &drmModeFreeResources}
      { if (!mHandle) perror("Couldn't retrieve DRM resources"); }

      explicit operator bool() const { return mHandle != nullptr; }

      class Connectors {
      private:
        drmModeRes const &mHandle;
      public:
        Connectors(drmModeRes const &handle) : mHandle{handle} {}
        auto begin() const { return mHandle.connectors; }
        auto end() const {
          return mHandle.connectors + mHandle.count_connectors;
        }
      };
      Connectors connectors() const { return {*mHandle.get()}; }

      class Crtcs {
        drmModeRes const &mHandle;
      public:
        Crtcs(drmModeRes const &handle) : mHandle{handle} {}
        auto begin() const { return mHandle.crtcs; }
        auto end() const { return mHandle.crtcs + mHandle.count_crtcs; }
      };
      Crtcs crtcs() const { return {*mHandle.get()}; }
    };

    std::optional<uint32_t> find_crtc_for_connector(
      Resources const &resources, Connector const &connector
    ) {
      for (uint32_t encoder_id : connector.encoders()) {
        Encoder encoder{mDescriptor.get(), encoder_id};
        if (!encoder) continue;
        int i = 0;
        for (uint32_t crtc_id : resources.crtcs()) {
          bool unused =
            mUnusedCrtcs.find(crtc_id) == mUnusedCrtcs.end()
          ;
          if (encoder.has_crtc(i) && unused) return crtc_id;
          ++i;
        }
      }
      return std::nullopt;
    }

    void set_mode(uint32_t /*connection_id*/, uint32_t /*crtc_id*/) {
    }

  public:
    DirectRenderingManager(
      FileDescriptor descriptor
    ) : mDescriptor{std::move(descriptor)}, mConnectorCrtcLookup{}
      , mUnusedCrtcs{}
    {
      // Populate the unused crtc list
      if (!mDescriptor) return;

      Resources resources{mDescriptor.get()};
      if (!resources) return;

      for (uint32_t crtc_id : resources.crtcs()) {
        mUnusedCrtcs.insert(crtc_id);
      }
    }

    explicit operator bool() const { return static_cast<bool>(mDescriptor); }

    void update_connections() {
      assert (*this);

      Resources resources{mDescriptor.get()};
      if (!resources) return;

      for (uint32_t connector_id : resources.connectors()) {
        Connector connector{mDescriptor.get(), connector_id};
        if (!connector) continue;

        auto it = mConnectorCrtcLookup.find(connector.id());
        if (it != mConnectorCrtcLookup.end()) {
          if (!connector.is_connected()) {
            // Someone unplugged it!
            mUnusedCrtcs.insert(it->second);
            mConnectorCrtcLookup.erase(it);
          }
        } else if (connector.is_connected()) {
          // Someone plugged it in!
          auto mode = connector.find_best_mode();
          if (!mode) continue;

          auto crtc_id = find_crtc_for_connector(resources, connector);
          if (!crtc_id) continue;

          set_mode(connector.id(), *crtc_id);
          mUnusedCrtcs.erase(*crtc_id);
          mConnectorCrtcLookup.emplace(connector.id(), *crtc_id);
        }
      }
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