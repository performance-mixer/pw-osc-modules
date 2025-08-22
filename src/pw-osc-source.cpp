#include <csignal>
#include <thread>
#include <lo/lo.h>
#include <boost/lockfree/spsc_queue.hpp>
#include <spa/control/control.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <pipewire/pipewire.h>

#define DEFAULT_BUFFER_SIZE 4096

/**
 *
 * The `osc source` listens for osc messages from the network and forwards them
 * over its sink.
 */
struct queue_message {
  uint8_t buffer[DEFAULT_BUFFER_SIZE];
  spa_pod *pod = nullptr;
};

using lock_free_queue = boost::lockfree::spsc_queue<
  queue_message, boost::lockfree::capacity<5>>;

struct stream_data {
  struct pw_properties *props;
  struct pw_stream *stream;
  struct spa_hook listener;
  lock_free_queue *queue;

  stream_data(const stream_data &other)
    : props(pw_properties_copy(other.props)), stream(other.stream),
      listener(other.listener), queue(other.queue) {}

  stream_data() = default;
};

struct impl {
  pw_context *context;
  pw_core *core;

  std::vector<stream_data> streams;
  std::unordered_map<std::string, lock_free_queue*> queues;
};

void process_stream(stream_data &stream, impl &impl) {
  struct pw_buffer *buffer = pw_stream_dequeue_buffer(stream.stream);
  if (buffer == nullptr) {
    pw_log_debug("%p: out of control buffers: %m", &impl);
    return;
  }

  spa_buffer *spa_buffer = buffer->buffer;
  if (!spa_buffer || spa_buffer->n_datas < 1) {
    pw_log_warn("control buffer has no data planes");
    return;
  }

  spa_data *data = &spa_buffer->datas[0];
  if (!data->data || !data->chunk || data->maxsize == 0) {
    pw_log_warn("invalid control buffer data");
    return;
  }

  spa_pod_builder b{};
  spa_pod_builder_init(&b, data->data, data->maxsize);

  spa_pod_frame seq_f{};
  spa_pod_builder_push_sequence(&b, &seq_f, 0);

  auto &queue = *stream.queue;

  if (queue.empty() == false) {
    auto &front = queue.front();
    const auto message_size = SPA_POD_SIZE(front.pod);

    struct spa_pod_builder_state state{};
    spa_pod_builder_get_state(&b, &state);

    if (const auto available = data->maxsize - state.offset; available <
      message_size) {
      pw_log_warn("not enough space, waiting for next buffer");
      goto end;
    }

    spa_pod_builder_control(&b, 0, SPA_CONTROL_Properties);
    spa_pod_builder_raw(&b, front.pod, message_size);

    queue.pop();
  }

end:
  const auto spa_sequence = spa_pod_builder_pop(&b, &seq_f);
  const uint32_t seq_size = SPA_POD_SIZE(spa_sequence);
  data->chunk->offset = 0;
  data->chunk->size = std::min<uint32_t>(seq_size, data->maxsize);
  data->chunk->stride = 0;
  pw_stream_queue_buffer(stream.stream, buffer);
}

static void process(void *d) {
  const auto impl_and_stream = static_cast<std::tuple<impl*, stream_data*>*>(d);
  const auto impl = std::get<0>(*impl_and_stream);
  const auto stream = std::get<1>(*impl_and_stream);
  process_stream(*stream, *impl);
}

void lo_error_handler(int num, const char *msg, const char *path) {}

int lo_message_handler(const char *path, const char *types, lo_arg **argv,
                       int argc, lo_message data, void *user_data) {
  const auto user_data_ = static_cast<std::tuple<std::string, impl*>*>(
    user_data);
  const auto &route_prefix = std::get<0>(*user_data_);
  auto &impl = std::get<1>(*user_data_);

  queue_message message{};
  spa_pod_builder builder{};
  spa_pod_builder_init(&builder, message.buffer, DEFAULT_BUFFER_SIZE);

  spa_pod_frame object_frame{};
  spa_pod_builder_push_object(&builder, &object_frame, SPA_TYPE_OBJECT_Props,
                              SPA_PARAM_Props);
  spa_pod_builder_prop(&builder, SPA_PROP_params, 0);

  spa_pod_frame struct_frame{};
  spa_pod_builder_push_struct(&builder, &struct_frame);

  for (int i = 0; i < argc; ++i) {
    const auto type = types[i];
    const auto arg = argv[i];

    std::string_view attribute_name_view(path);

    attribute_name_view.remove_prefix(route_prefix.size() + 1);
    switch (type) {
    case 'i':
      spa_pod_builder_string(&builder,
                             std::string(attribute_name_view).c_str());
      spa_pod_builder_int(&builder, arg->i);
      break;
    case 'f':
      spa_pod_builder_string(&builder,
                             std::string(attribute_name_view).c_str());
      spa_pod_builder_float(&builder, arg->f);
      break;
    case 's':
    case 'S':
      spa_pod_builder_string(&builder,
                             std::string(attribute_name_view).c_str());
      spa_pod_builder_string(&builder, &arg->s);
      break;
    case 'h':
      spa_pod_builder_string(&builder,
                             std::string(attribute_name_view).c_str());
      spa_pod_builder_long(&builder, arg->h);
      break;
    case 'd':
      spa_pod_builder_string(&builder,
                             std::string(attribute_name_view).c_str());
      spa_pod_builder_double(&builder, arg->d);
      break;
    default:
      break;
    }
  }

  const auto &queue = impl->queues[route_prefix];
  queue->push(message);

  return 0;
}

static spa_pod *make_buffers_param(spa_pod_builder *b, uint32_t size_bytes) {
  spa_pod_frame f{};
  spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_ParamBuffers,
                              SPA_PARAM_Buffers);
  spa_pod_builder_add(b, SPA_PARAM_BUFFERS_buffers,
                      SPA_POD_CHOICE_RANGE_Int(1, 1, 1),
                      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                      SPA_PARAM_BUFFERS_size,
                      SPA_POD_Int(static_cast<int>(size_bytes)),
                      SPA_PARAM_BUFFERS_stride, SPA_POD_Int(0),
                      SPA_PARAM_BUFFERS_align, SPA_POD_Int(16), 0);
  return static_cast<spa_pod*>(spa_pod_builder_pop(b, &f));
}

static spa_pod *make_control_format(spa_pod_builder *b) {
  spa_pod_frame f{};
  // Build: SPA_PARAM_EnumFormat with mediaType=Application, mediaSubtype=Control
  spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(b, SPA_FORMAT_mediaType,
                      SPA_POD_Id(SPA_MEDIA_TYPE_application),
                      SPA_FORMAT_mediaSubtype,
                      SPA_POD_Id(SPA_MEDIA_SUBTYPE_control), 0);
  return static_cast<spa_pod*>(spa_pod_builder_pop(b, &f));
}

static pw_main_loop *g_loop = nullptr;

static void handle_signal(int) {
  if (g_loop) pw_main_loop_quit(g_loop);
}

struct stream_description {
  std::string node_name;
  std::string description;
  std::string port_alias;
  bool autoconnect;
  std::string target_object;
  lock_free_queue *queue;
};

using stream_descriptions = std::vector<stream_description>;

void stream_thread(const stream_descriptions &stream_descriptions, impl &data,
                   int argc, char **argv) {
  const std::string node_base_name = "osc-source";
  const std::string port_alias = "osc_source:control_out";

  pw_init(&argc, &argv);

  g_loop = pw_main_loop_new(nullptr);
  if (!g_loop) {
    std::fprintf(stderr, "Failed to create pw_main_loop\n");
    return;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  data.context = pw_context_new(pw_main_loop_get_loop(g_loop), nullptr, 0);
  if (!data.context) {
    std::fprintf(stderr, "Failed to create pw_context\n");
    pw_main_loop_destroy(g_loop);
    return;
  }

  data.core = pw_context_connect(data.context, nullptr, 0);
  if (!data.core) {
    std::fprintf(stderr, "Failed to connect pw_core\n");
    pw_context_destroy(data.context);
    pw_main_loop_destroy(g_loop);
    return;
  }

  for (const auto &stream_description : stream_descriptions) {
    static struct pw_stream_events control_stream_events = {
      .version = PW_VERSION_STREAM_EVENTS, .process = process,
    };
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *params[2];
    params[0] = make_control_format(&builder);
    params[1] = make_buffers_param(&builder, 1024);

    pw_properties *out_props = pw_properties_new(
      PW_KEY_APP_NAME, "pmx", PW_KEY_NODE_NAME,
      stream_description.node_name.c_str(), PW_KEY_NODE_DESCRIPTION,
      stream_description.description.c_str(), PW_KEY_MEDIA_CLASS,
      "Stream/Output/Control", PW_KEY_NODE_AUTOCONNECT,
      stream_description.autoconnect ? "true" : "false", PW_KEY_PORT_ALIAS,
      stream_description.port_alias.c_str(), PW_KEY_LINK_PASSIVE, "false",
      PW_KEY_TARGET_OBJECT, "My Control In", nullptr);

    data.streams.emplace_back();
    auto &stream = data.streams.back();
    stream.stream = pw_stream_new(data.core, "pmx-control-out", out_props);
    stream.queue = stream_description.queue;

    if (!stream.stream) {
      std::fprintf(stderr, "Failed to create output stream\n");
      pw_core_disconnect(data.core);
      pw_context_destroy(data.context);
      pw_main_loop_destroy(g_loop);
      return;
    }

    auto callback_data = std::make_tuple(&data, &stream);
    pw_stream_add_listener(stream.stream, &stream.listener,
                           &control_stream_events, &callback_data);

    int res = pw_stream_connect(stream.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                static_cast<pw_stream_flags>(
                                  PW_STREAM_FLAG_AUTOCONNECT |
                                  PW_STREAM_FLAG_MAP_BUFFERS), params, 2);
    if (res < 0) {
      std::fprintf(stderr, "Failed to connect output stream: %d\n", res);
      pw_stream_destroy(stream.stream);
      pw_core_disconnect(data.core);
      pw_context_destroy(data.context);
      pw_main_loop_destroy(g_loop);
      return;
    }
  }

  pw_main_loop_run(g_loop);

  for (const auto &stream : data.streams) {
    pw_stream_destroy(stream.stream);
  }

  pw_core_disconnect(data.core);
  pw_context_destroy(data.context);
  pw_main_loop_destroy(g_loop);
  g_loop = nullptr;
}

int main(int argc, char **argv) {
  impl impl{};

  lock_free_queue queue{};
  impl.queues["/cmp/1"] = &queue;

  stream_descriptions stream_descriptions{
    {
      "osc-source-1", "PMX Control Port", "osc_source:control_out_1", true,
      "My Control In", &queue,
    }
  };

  std::thread t(stream_thread, std::ref(stream_descriptions), std::ref(impl),
                argc, argv);

  lo_server server = lo_server_new("1337", lo_error_handler);
  const auto data_tuple = std::make_tuple("/cmp/1", &impl);
  lo_server_add_method(server, "/cmp/1", nullptr, lo_message_handler,
                       &data_tuple);

  t.join();
  return 0;
}
