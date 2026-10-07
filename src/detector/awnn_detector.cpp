#include "aibox/awnn_detector.hpp"
#include <awnn_lib.h>
#include <vip_lite.h>
#include <array>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace aibox
{
    namespace
    {
        void check(vip_status_e s, const char *what)
        {
            if (s != VIP_SUCCESS)
                throw std::runtime_error(std::string(what) + " failed with VIP status " + std::to_string(static_cast<int>(s)));
        }
        struct ModelInfo
        {
            std::vector<int> output_strides;
        };
        ModelInfo inspect_model(const std::string &path)
        {
            check(vip_init(), "vip_init");
            vip_network net = VIP_NULL;
            try
            {
                check(vip_create_network(path.c_str(), 0, VIP_CREATE_NETWORK_FROM_FILE, &net), "vip_create_network(metadata)");
                vip_uint32_t input_count = 0, output_count = 0;
                check(vip_query_network(net, VIP_NETWORK_PROP_INPUT_COUNT, &input_count), "query input count");
                check(vip_query_network(net, VIP_NETWORK_PROP_OUTPUT_COUNT, &output_count), "query output count");
                if (input_count != 1 || output_count != 3)
                    throw std::runtime_error("Expected one input and three YOLOv5 output heads");
                vip_uint32_t dims = 0;
                std::array<vip_uint32_t, 6> sizes{};
                vip_enum fmt = 0;
                check(vip_query_input(net, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &dims), "query input dimensions");
                check(vip_query_input(net, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, sizes.data()), "query input sizes");
                check(vip_query_input(net, 0, VIP_BUFFER_PROP_DATA_FORMAT, &fmt), "query input format");
                const bool input_nchw = dims == 4 && sizes[0] == 1 && sizes[1] == 3 && sizes[2] == 640 && sizes[3] == 640;
                // This VIP 2.0.3.2 runtime reports dimensions least-significant-first: W,H,C,N.
                // The supplied AW YOLOv5 preprocessor writes planar CHW bytes for this model.
                const bool input_vip_reversed = dims == 4 && sizes[0] == 640 && sizes[1] == 640 && sizes[2] == 3 && sizes[3] == 1;
                if ((!input_nchw && !input_vip_reversed) || fmt != VIP_BUFFER_FORMAT_UINT8)
                {
                    std::ostringstream msg;
                    msg << "NBG input metadata: dims=" << dims << " sizes=[";
                    for (vip_uint32_t i = 0; i < dims && i < sizes.size(); ++i)
                    {
                        if (i)
                            msg << ',';
                        msg << sizes[i];
                    }
                    msg << "] format=" << fmt << " (UINT8=" << VIP_BUFFER_FORMAT_UINT8 << ")";
                    throw std::runtime_error(msg.str());
                }
                ModelInfo info;
                std::array<bool, 3> seen{};
                for (vip_uint32_t i = 0; i < output_count; ++i)
                {
                    dims = 0;
                    sizes.fill(0);
                    check(vip_query_output(net, i, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &dims), "query output dimensions");
                    check(vip_query_output(net, i, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, sizes.data()), "query output sizes");
                    if (dims != 5)
                    {
                        std::ostringstream msg;
                        msg << "NBG output metadata: dims=" << dims;
                        throw std::runtime_error(msg.str());
                    }
                    vip_uint32_t h = 0, w = 0;
                    const bool direct = sizes[0] == 1 && sizes[1] == 3 && sizes[4] == 85;
                    const bool reversed = sizes[0] == 85 && sizes[3] == 3 && sizes[4] == 1;
                    if (direct)
                    {
                        h = sizes[2];
                        w = sizes[3];
                    }
                    else if (reversed)
                    {
                        h = sizes[1];
                        w = sizes[2];
                    }
                    else
                    {
                        std::ostringstream msg;
                        msg << "NBG output metadata: sizes=[";
                        for (vip_uint32_t j = 0; j < dims; ++j)
                        {
                            if (j)
                                msg << ',';
                            msg << sizes[j];
                        }
                        msg << "] (expected [1,3,H,W,85] or reversed [85,W,H,3,1])";
                        throw std::runtime_error(msg.str());
                    }
                    if (h != w || (h != 80 && h != 40 && h != 20))
                        throw std::runtime_error("Unexpected YOLOv5 head grid in NBG output");
                    const int stride = 640 / static_cast<int>(h);
                    const int slot = stride == 8 ? 0 : (stride == 16 ? 1 : 2);
                    if (seen[slot])
                        throw std::runtime_error("Duplicate YOLOv5 head grid");
                    seen[slot] = true;
                    info.output_strides.push_back(stride);
                }
                check(vip_destroy_network(net), "vip_destroy_network(metadata)");
                net = VIP_NULL;
                check(vip_destroy(), "vip_destroy(metadata)");
                return info;
            }
            catch (...)
            {
                if (net != VIP_NULL)
                    vip_destroy_network(net);
                vip_destroy();
                throw;
            }
        }
    }
    struct AwnnDetector::Impl
    {
        Awnn_Context_t *context{};
        std::vector<int> strides;
        bool initialized{};
    };
    AwnnDetector::AwnnDetector(const std::string &path) : impl_(std::make_unique<Impl>())
    {
        auto m = inspect_model(path);
        impl_->strides = std::move(m.output_strides);
        awnn_init();
        impl_->initialized = true;
        impl_->context = awnn_create(path.c_str());
        if (!impl_->context)
        {
            awnn_uninit();
            impl_->initialized = false;
            throw std::runtime_error("awnn_create returned null");
        }
    }
    AwnnDetector::~AwnnDetector()
    {
        if (impl_)
        {
            if (impl_->context)
                awnn_destroy(impl_->context);
            if (impl_->initialized)
                awnn_uninit();
        }
    }
    std::vector<Detection> AwnnDetector::detect(const cv::Mat &frame, float threshold,
                                               float nms_iou,
                                               const std::vector<int>& class_ids)
    {
        auto prepared = prepare_yolov5(frame, 640);
        void *input[] = {prepared.chw.data()};
        awnn_set_input_buffers(impl_->context, input);
        awnn_run(impl_->context);
        float **outputs = awnn_get_output_buffers(impl_->context);
        if (!outputs)
            throw std::runtime_error("AWNN returned null output pointer");
        static constexpr std::array<std::size_t, 3> count{3u * 80 * 80 * 85, 3u * 40 * 40 * 85, 3u * 20 * 20 * 85};
        std::vector<YoloHead> heads;
        heads.reserve(3);
        // The AWNN wrapper returns buffers in queried NBG output-index order. The order-to-stride
        // association established by VIP metadata is the same index order; decoder sorts by stride.
        for (std::size_t i = 0; i < 3; ++i)
        {
            if (!outputs[i])
                throw std::runtime_error("AWNN output tensor is null");
            heads.push_back({impl_->strides[i], outputs[i], count[i]});
        }
        return decode_yolov5(heads, prepared.transform, threshold, nms_iou, class_ids);
    }
}
