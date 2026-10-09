/**
 * @file tests/unit/platform/linux/test_wayland.cpp
 * @brief Test Wayland output mode selection.
 */
#ifdef SUNSHINE_BUILD_WAYLAND
  // standard includes
  #include <array>
  #include <cerrno>
  #include <cstdlib>

  // system includes
  #include <drm_fourcc.h>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <unistd.h>

  // test includes
  #include "../../../tests_common.h"

  // local includes
  #include <src/config.h>
  #include <src/platform/linux/wayland.h>

namespace {
  struct fake_gbm_bo_t {
    int plane_count {};
    int failed_plane {-1};
    std::uint64_t modifier {};
    std::array<std::uint32_t, 4> strides {};
    std::array<std::uint32_t, 4> offsets {};
    std::array<int, 4> exported_fds {-1, -1, -1, -1};
  };

  fake_gbm_bo_t &fake_bo(gbm_bo *bo) {
    return *static_cast<fake_gbm_bo_t *>(static_cast<void *>(bo));
  }

  int get_plane_count(gbm_bo *bo) {
    return fake_bo(bo).plane_count;
  }

  int get_fd_for_plane(gbm_bo *bo, int plane) {
    auto &fake = fake_bo(bo);
    if (plane == fake.failed_plane) {
      return -1;
    }

    fake.exported_fds[plane] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    return fake.exported_fds[plane];
  }

  std::uint32_t get_stride_for_plane(gbm_bo *bo, int plane) {
    return fake_bo(bo).strides[plane];
  }

  std::uint32_t get_offset(gbm_bo *bo, int plane) {
    return fake_bo(bo).offsets[plane];
  }

  std::uint64_t get_modifier(gbm_bo *bo) {
    return fake_bo(bo).modifier;
  }

  const wl::gbm_bo_accessors_t fake_accessors {
    .get_plane_count = get_plane_count,
    .get_fd_for_plane = get_fd_for_plane,
    .get_stride_for_plane = get_stride_for_plane,
    .get_offset = get_offset,
    .get_modifier = get_modifier,
  };
}  // namespace

TEST(WaylandMonitorTest, IgnoresNonCurrentModesAroundCurrentMode) {
  wl::monitor_t monitor {nullptr};

  monitor.wl_mode(nullptr, WL_OUTPUT_MODE_PREFERRED, 1280, 720, 60000);
  monitor.wl_mode(nullptr, WL_OUTPUT_MODE_CURRENT, 1920, 1080, 60000);
  monitor.wl_mode(nullptr, 0, 2560, 1440, 60000);

  EXPECT_EQ(monitor.viewport.width, 1920);
  EXPECT_EQ(monitor.viewport.height, 1080);
}

TEST(WaylandMonitorTest, UpdatesCurrentMode) {
  wl::monitor_t monitor {nullptr};

  monitor.wl_mode(nullptr, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1920, 1080, 60000);
  monitor.wl_mode(nullptr, WL_OUTPUT_MODE_CURRENT, 2560, 1440, 120000);

  EXPECT_EQ(monitor.viewport.width, 2560);
  EXPECT_EQ(monitor.viewport.height, 1440);
}

TEST(WaylandCaptureTest, UsesVramForVaapi) {
  EXPECT_TRUE(wl::use_vram_capture(platf::mem_type_e::vaapi));
}

TEST(WaylandCaptureTest, UsesSystemMemoryForSoftwareEncoding) {
  EXPECT_FALSE(wl::use_vram_capture(platf::mem_type_e::system));
}

TEST(WaylandCaptureTest, UsesVramForCudaOnlyWhenCudaSupportIsBuilt) {
  #ifdef SUNSHINE_BUILD_CUDA
  EXPECT_TRUE(wl::use_vram_capture(platf::mem_type_e::cuda));
  #else
  EXPECT_FALSE(wl::use_vram_capture(platf::mem_type_e::cuda));
  #endif
}

TEST(WaylandInterfaceTest, RecordsOnlyExplicitDmabufModifiers) {
  constexpr std::uint32_t format = DRM_FORMAT_XRGB8888;
  constexpr std::uint64_t explicit_modifier = 0x100000000000004;
  wl::interface_t interface;

  interface.dmabuf_modifier(nullptr, format, DRM_FORMAT_MOD_INVALID >> 32, DRM_FORMAT_MOD_INVALID & 0xffffffff);
  interface.dmabuf_modifier(nullptr, format, explicit_modifier >> 32, explicit_modifier & 0xffffffff);

  const auto modifiers = interface.supported_modifiers.find(format);
  ASSERT_NE(modifiers, interface.supported_modifiers.end());
  ASSERT_EQ(modifiers->second.size(), 1);
  EXPECT_EQ(modifiers->second.front(), explicit_modifier);
}

TEST(WaylandCaptureTest, ExportsEveryGbmBufferPlane) {
  fake_gbm_bo_t bo {
    .plane_count = 2,
    .modifier = 0x100000000000004,
    .strides = {10240, 256},
    .offsets = {0, 0x800000},
  };
  wl::frame_t frame;

  const auto plane_count = wl::export_gbm_bo_planes(static_cast<gbm_bo *>(static_cast<void *>(&bo)), frame, fake_accessors);

  ASSERT_EQ(plane_count, 2);
  EXPECT_EQ(frame.sd.modifier, bo.modifier);
  for (std::size_t plane = 0; plane < *plane_count; ++plane) {
    EXPECT_EQ(frame.sd.fds[plane], bo.exported_fds[plane]);
    EXPECT_EQ(frame.sd.pitches[plane], bo.strides[plane]);
    EXPECT_EQ(frame.sd.offsets[plane], bo.offsets[plane]);
  }
  EXPECT_EQ(frame.sd.fds[2], -1);
  EXPECT_EQ(frame.sd.fds[3], -1);

  frame.destroy();
}

TEST(WaylandCaptureTest, RejectsUnsupportedGbmPlaneCounts) {
  wl::frame_t frame;
  fake_gbm_bo_t empty_bo {};
  fake_gbm_bo_t oversized_bo {
    .plane_count = 5,
  };

  EXPECT_FALSE(wl::export_gbm_bo_planes(static_cast<gbm_bo *>(static_cast<void *>(&empty_bo)), frame, fake_accessors));
  EXPECT_FALSE(wl::export_gbm_bo_planes(static_cast<gbm_bo *>(static_cast<void *>(&oversized_bo)), frame, fake_accessors));
}

TEST(WaylandCaptureTest, ClosesExportedPlanesAfterLaterPlaneFails) {
  fake_gbm_bo_t bo {
    .plane_count = 2,
    .failed_plane = 1,
  };
  wl::frame_t frame;

  EXPECT_FALSE(wl::export_gbm_bo_planes(static_cast<gbm_bo *>(static_cast<void *>(&bo)), frame, fake_accessors));
  ASSERT_GE(bo.exported_fds[0], 0);
  const auto fd_status = fcntl(bo.exported_fds[0], F_GETFD);
  const auto fd_error = errno;
  EXPECT_EQ(fd_status, -1);
  EXPECT_EQ(fd_error, EBADF);
  EXPECT_EQ(frame.sd.fds[0], -1);
}

namespace {
  /**
   * @brief Wayland client connected to a socket peer that records requests without a compositor.
   */
  class screencopy_wire_t {
  public:
    /**
     * @brief Connect the recording peer and bind a version-three screencopy manager.
     */
    screencopy_wire_t() {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        return;
      }
      peer = sockets[1];
      display.reset(wl_display_connect_to_fd(sockets[0]));
      if (!display) {
        return;
      }
      auto registry = wl_display_get_registry(display.get());
      manager = static_cast<zwlr_screencopy_manager_v1 *>(wl_registry_bind(registry, 1, &zwlr_screencopy_manager_v1_interface, 3));
      output = static_cast<wl_output *>(wl_registry_bind(registry, 2, &wl_output_interface, 2));
      dmabuf = static_cast<zwp_linux_dmabuf_v1 *>(wl_registry_bind(registry, 3, &zwp_linux_dmabuf_v1_interface, 3));
      wl_registry_destroy(registry);
      requests();
    }

    /**
     * @brief Release the recording connection.
     */
    ~screencopy_wire_t() {
      if (manager) {
        zwlr_screencopy_manager_v1_destroy(manager);
      }
      if (dmabuf) {
        zwp_linux_dmabuf_v1_destroy(dmabuf);
      }
      if (output) {
        wl_output_destroy(output);
      }
      if (peer >= 0) {
        close(peer);
      }
    }

    /**
     * @brief Return object IDs and opcodes sent since the previous read.
     * @return Recorded requests in wire order.
     */
    std::vector<std::pair<std::uint32_t, std::uint16_t>> requests() {
      wl_display_flush(display.get());
      std::array<std::uint32_t, 256> wire {};
      const auto bytes = recv(peer, wire.data(), sizeof(wire), MSG_DONTWAIT);
      std::vector<std::pair<std::uint32_t, std::uint16_t>> result;
      for (int offset = 0; offset < bytes / 4;) {
        const auto header = wire[offset + 1];
        result.emplace_back(wire[offset], header & 0xffff);
        if (wire[offset] == wl_proxy_get_id(reinterpret_cast<wl_proxy *>(manager)) && (header & 0xffff) == ZWLR_SCREENCOPY_MANAGER_V1_CAPTURE_OUTPUT) {
          frame_id = wire[offset + 2];
        }
        if (wire[offset] == wl_proxy_get_id(reinterpret_cast<wl_proxy *>(dmabuf)) && (header & 0xffff) == ZWP_LINUX_DMABUF_V1_CREATE_PARAMS) {
          params_ids.push_back(wire[offset + 2]);
        }
        offset += (header >> 16) / 4;
      }
      return result;
    }

    /**
     * @brief Deliver a compositor failure event to the latest capture request.
     */
    void fail_frame() {
      const std::array<std::uint32_t, 2> event {frame_id, (8u << 16) | 3u};
      ASSERT_EQ(send(peer, event.data(), sizeof(event), 0), sizeof(event));
      ASSERT_GE(wl_display_dispatch(display.get()), 0);
    }

    /**
     * @brief Deliver a compositor ready event to the latest capture request.
     */
    void ready_frame() {
      const std::array<std::uint32_t, 5> event {frame_id, (20u << 16) | 2u, 0, 1, 0};
      ASSERT_EQ(send(peer, event.data(), sizeof(event), 0), sizeof(event));
      ASSERT_GE(wl_display_dispatch(display.get()), 0);
    }

    /**
     * @brief Advertise a DMA-BUF and finish describing the latest frame.
     * @param fourcc DRM format code to advertise.
     */
    void describe_frame(std::uint32_t fourcc = DRM_FORMAT_XRGB8888) {
      const std::array<std::uint32_t, 7> event {frame_id, (20u << 16) | 5u, fourcc, 2752, 2064, frame_id, (8u << 16) | 6u};
      ASSERT_EQ(send(peer, event.data(), sizeof(event), 0), sizeof(event));
      ASSERT_GE(wl_display_dispatch(display.get()), 0);
    }

    /**
     * @brief Advertise only a shared-memory buffer and finish describing the latest frame.
     */
    void describe_shm_frame() {
      const std::array<std::uint32_t, 8> event {frame_id, (24u << 16) | 0u, DRM_FORMAT_XRGB8888, 2752, 2064, 2752 * 4, frame_id, (8u << 16) | 6u};
      ASSERT_EQ(send(peer, event.data(), sizeof(event), 0), sizeof(event));
      ASSERT_GE(wl_display_dispatch(display.get()), 0);
    }

    /**
     * @brief Finish describing the latest frame without advertising any buffer type.
     */
    void describe_empty_frame() {
      const std::array<std::uint32_t, 2> event {frame_id, (8u << 16) | 6u};
      ASSERT_EQ(send(peer, event.data(), sizeof(event), 0), sizeof(event));
      ASSERT_GE(wl_display_dispatch(display.get()), 0);
    }

    /**
     * @brief Deliver a buffer creation failure to the latest parameter request.
     */
    void fail_buffer() {
      const std::array<std::uint32_t, 2> event {params_ids.back(), (8u << 16) | 1u};
      ASSERT_EQ(send(peer, event.data(), sizeof(event), 0), sizeof(event));
      ASSERT_GE(wl_display_dispatch(display.get()), 0);
    }

    /**
     * @brief Complete asynchronous buffer creation for the latest parameter request.
     */
    void create_buffer() {
      const std::array<std::uint32_t, 3> event {params_ids.back(), 12u << 16, 0xff000000u};
      ASSERT_EQ(send(peer, event.data(), sizeof(event), 0), sizeof(event));
      ASSERT_GE(wl_display_dispatch(display.get()), 0);
    }

    zwp_linux_dmabuf_v1 *dmabuf {nullptr};  ///< DMA-BUF interface used by allocation tests.
    std::vector<std::uint32_t> params_ids;  ///< Requested asynchronous buffer parameter IDs.
    std::uint32_t frame_id {};  ///< Most recently requested screencopy frame ID.
    wl::display_internal_t display;  ///< Client connection used by the capture object.
    zwlr_screencopy_manager_v1 *manager {nullptr};  ///< Bound screencopy manager.
    wl_output *output {nullptr};  ///< Output proxy used in capture requests.
    int peer {-1};  ///< Peer socket that receives client requests.
  };
}  // namespace

TEST(WaylandCaptureTest, KeepsOneRequestAcrossTimeoutRetries) {
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  wl::dmabuf_t capture;

  capture.listen(wire.manager, nullptr, nullptr, wire.output);
  auto first = wire.requests();
  ASSERT_EQ(first.size(), 1);
  EXPECT_EQ(first.front().first, wl_proxy_get_id(reinterpret_cast<wl_proxy *>(wire.manager)));
  EXPECT_EQ(first.front().second, ZWLR_SCREENCOPY_MANAGER_V1_CAPTURE_OUTPUT);

  // No terminal callback has arrived, as after a snapshot timeout.
  capture.listen(wire.manager, nullptr, nullptr, wire.output);
  EXPECT_TRUE(wire.requests().empty());
  EXPECT_EQ(capture.status, wl::dmabuf_t::WAITING);
}

TEST(WaylandCaptureTest, CancelsPendingRequestOnDestruction) {
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  {
    wl::dmabuf_t capture;
    capture.listen(wire.manager, nullptr, nullptr, wire.output);
    ASSERT_EQ(wire.requests().size(), 1);
  }

  const auto requests = wire.requests();
  ASSERT_EQ(requests.size(), 1);
  EXPECT_EQ(requests.front().second, ZWLR_SCREENCOPY_FRAME_V1_DESTROY);
}

TEST(WaylandCaptureTest, StartsNewRequestAfterCompositorFailure) {
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  wl::dmabuf_t capture;
  capture.listen(wire.manager, nullptr, nullptr, wire.output);
  ASSERT_EQ(wire.requests().size(), 1);

  wire.fail_frame();
  EXPECT_EQ(capture.status, wl::dmabuf_t::REINIT);
  ASSERT_EQ(wire.requests().size(), 1);

  capture.listen(wire.manager, nullptr, nullptr, wire.output);
  const auto requests = wire.requests();
  ASSERT_EQ(requests.size(), 1);
  EXPECT_EQ(requests.front().second, ZWLR_SCREENCOPY_MANAGER_V1_CAPTURE_OUTPUT);
}

TEST(WaylandCaptureTest, StartsNewRequestAfterReadyFrame) {
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  wl::dmabuf_t capture;
  capture.listen(wire.manager, nullptr, nullptr, wire.output);
  ASSERT_EQ(wire.requests().size(), 1);

  wire.ready_frame();
  EXPECT_EQ(capture.status, wl::dmabuf_t::READY);
  ASSERT_EQ(wire.requests().size(), 1);

  capture.listen(wire.manager, nullptr, nullptr, wire.output);
  ASSERT_EQ(wire.requests().size(), 1);
}

TEST(WaylandCaptureTest, DoesNotCancelCompletedRequestAgain) {
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  {
    wl::dmabuf_t capture;
    capture.listen(wire.manager, nullptr, nullptr, wire.output);
    ASSERT_EQ(wire.requests().size(), 1);
    wire.fail_frame();
    ASSERT_EQ(wire.requests().size(), 1);
  }
  EXPECT_TRUE(wire.requests().empty());
}

TEST(WaylandCaptureTest, CancelsRequestWhenNoBufferTypeIsAdvertised) {
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  wl::dmabuf_t capture;
  capture.listen(wire.manager, wire.dmabuf, nullptr, wire.output);
  ASSERT_EQ(wire.requests().size(), 1);

  wire.describe_empty_frame();
  EXPECT_EQ(capture.status, wl::dmabuf_t::REINIT);
  const auto requests = wire.requests();
  ASSERT_EQ(requests.size(), 1);
  EXPECT_EQ(requests.front().first, wire.frame_id);
  EXPECT_EQ(requests.front().second, ZWLR_SCREENCOPY_FRAME_V1_DESTROY);
}

TEST(WaylandCaptureTest, CancelsRequestWhenOnlySharedMemoryIsAdvertised) {
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  wl::dmabuf_t capture;
  capture.listen(wire.manager, wire.dmabuf, nullptr, wire.output);
  ASSERT_EQ(wire.requests().size(), 1);

  wire.describe_shm_frame();
  EXPECT_EQ(capture.status, wl::dmabuf_t::REINIT);
  const auto requests = wire.requests();
  ASSERT_EQ(requests.size(), 1);
  EXPECT_EQ(requests.front().second, ZWLR_SCREENCOPY_FRAME_V1_DESTROY);
}

TEST(WaylandCaptureTest, CancelsRequestWhenRenderNodeCannotBeOpened) {
  auto previous_adapter = std::exchange(config::video.adapter_name, "/nonexistent/render-node");
  auto restore_adapter = util::fail_guard([&] {
    config::video.adapter_name = previous_adapter;
  });
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  wl::dmabuf_t capture;
  capture.listen(wire.manager, wire.dmabuf, nullptr, wire.output);
  ASSERT_EQ(wire.requests().size(), 1);

  wire.describe_frame();
  EXPECT_EQ(capture.status, wl::dmabuf_t::REINIT);
  const auto requests = wire.requests();
  ASSERT_EQ(requests.size(), 1);
  EXPECT_EQ(requests.front().second, ZWLR_SCREENCOPY_FRAME_V1_DESTROY);
}

TEST(WaylandCaptureTest, ReleasesRequestAndBufferAfterSuccessfulCapture) {
  const char *render_node = std::getenv("SUNSHINE_TEST_WAYLAND_RENDER_NODE");
  if (!render_node) {
    GTEST_SKIP() << "Set SUNSHINE_TEST_WAYLAND_RENDER_NODE to opt in to real GBM allocation";
  }
  auto previous_adapter = std::exchange(config::video.adapter_name, render_node);
  auto restore_adapter = util::fail_guard([&] {
    config::video.adapter_name = previous_adapter;
  });
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  {
    wl::dmabuf_t capture;
    const std::map<std::uint32_t, std::vector<std::uint64_t>> modifiers {{DRM_FORMAT_XRGB8888, {DRM_FORMAT_MOD_LINEAR}}};
    capture.listen(wire.manager, wire.dmabuf, &modifiers, wire.output);
    ASSERT_EQ(wire.requests().size(), 1);
    for (int retry = 0; retry < 4; ++retry) {
      capture.listen(wire.manager, wire.dmabuf, &modifiers, wire.output);
      EXPECT_TRUE(wire.requests().empty());
    }
    wire.describe_frame();
    wire.requests();
    ASSERT_EQ(wire.params_ids.size(), 1);
    wire.create_buffer();
    wire.requests();
    wire.ready_frame();
    EXPECT_EQ(capture.status, wl::dmabuf_t::READY);
    const auto terminal = wire.requests();
    EXPECT_EQ(std::count_if(terminal.begin(), terminal.end(), [](const auto &request) {
                return request.first == 0xff000000u;
              }),
              1);
  }
}

TEST(WaylandCaptureTest, CancelsBufferParamsWhenFrameFails) {
  const char *render_node = std::getenv("SUNSHINE_TEST_WAYLAND_RENDER_NODE");
  if (!render_node) {
    GTEST_SKIP() << "Set SUNSHINE_TEST_WAYLAND_RENDER_NODE to opt in to real GBM allocation";
  }
  auto previous_adapter = std::exchange(config::video.adapter_name, render_node);
  auto restore_adapter = util::fail_guard([&] {
    config::video.adapter_name = previous_adapter;
  });
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  wl::dmabuf_t capture;
  capture.listen(wire.manager, wire.dmabuf, nullptr, wire.output);
  ASSERT_EQ(wire.requests().size(), 1);
  wire.describe_frame();
  wire.requests();
  ASSERT_EQ(wire.params_ids.size(), 1);

  wire.fail_frame();
  EXPECT_EQ(capture.status, wl::dmabuf_t::REINIT);
  const auto requests = wire.requests();
  ASSERT_EQ(requests.size(), 2);
  EXPECT_EQ(requests[0].first, wire.params_ids.front());
  EXPECT_EQ(requests[1].first, wire.frame_id);
}

TEST(WaylandCaptureTest, CancelsRequestWhenBufferAllocationFails) {
  const char *render_node = std::getenv("SUNSHINE_TEST_WAYLAND_RENDER_NODE");
  if (!render_node) {
    GTEST_SKIP() << "Set SUNSHINE_TEST_WAYLAND_RENDER_NODE to opt in to real GBM allocation";
  }
  auto previous_adapter = std::exchange(config::video.adapter_name, render_node);
  auto restore_adapter = util::fail_guard([&] {
    config::video.adapter_name = previous_adapter;
  });
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  wl::dmabuf_t capture;
  capture.listen(wire.manager, wire.dmabuf, nullptr, wire.output);
  ASSERT_EQ(wire.requests().size(), 1);

  wire.describe_frame(DRM_FORMAT_INVALID);
  EXPECT_EQ(capture.status, wl::dmabuf_t::REINIT);
  const auto requests = wire.requests();
  ASSERT_EQ(requests.size(), 1);
  EXPECT_EQ(requests.front().second, ZWLR_SCREENCOPY_FRAME_V1_DESTROY);
}

TEST(WaylandCaptureTest, CancelsRequestWhenBufferCreationFails) {
  const char *render_node = std::getenv("SUNSHINE_TEST_WAYLAND_RENDER_NODE");
  if (!render_node) {
    GTEST_SKIP() << "Set SUNSHINE_TEST_WAYLAND_RENDER_NODE to opt in to real GBM allocation";
  }
  auto previous_adapter = std::exchange(config::video.adapter_name, render_node);
  auto restore_adapter = util::fail_guard([&] {
    config::video.adapter_name = previous_adapter;
  });
  screencopy_wire_t wire;
  ASSERT_TRUE(wire.display);
  wl::dmabuf_t capture;
  capture.listen(wire.manager, wire.dmabuf, nullptr, wire.output);
  ASSERT_EQ(wire.requests().size(), 1);
  wire.describe_frame();
  wire.requests();
  ASSERT_EQ(wire.params_ids.size(), 1);

  wire.fail_buffer();
  EXPECT_EQ(capture.status, wl::dmabuf_t::REINIT);
  const auto requests = wire.requests();
  ASSERT_EQ(requests.size(), 2);
  EXPECT_EQ(requests[0].first, wire.params_ids.front());
  EXPECT_EQ(requests[1].first, wire.frame_id);
}
#endif
