#include "GstAppSinkImpl.h"
#include "GstPipelineImpl.h"
#include "GstSampleImpl.h"

extern "C"
{
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video-format.h>
}

#include <vector>
#include <mutex>

class FGstAppSinkImpl : public IGstAppSink
{
public:
    FGstAppSinkImpl() {}
    ~FGstAppSinkImpl() { Disconnect(); }
    virtual void Destroy();

    virtual bool Connect(IGstPipeline *Pipeline, const char *ElementName, IGstAppSinkCallback *Callback);
    virtual void Disconnect();
    virtual void ReleaseSample(IGstSample *Sample);

    bool IsKlv() { return m_IsKlv; }

    GstFlowReturn OnNewSample(GstElement *Sink);
    IGstSample *AllocSample();
    void DeallocSample(IGstSample *Sample);
    void DeallocSamplePool();

private:
    std::string m_Name;
    IGstAppSinkCallback *m_Callback = nullptr;
    GstElement *m_AppSink = nullptr;

    int m_IsKlv = false;
    int m_Format = GST_VIDEO_FORMAT_UNKNOWN;
    int m_Width = 0;
    int m_Height = 0;
    gint m_Framerate = 1;

    std::vector<IGstSample *> m_SamplePool;
    std::mutex m_SampleMx;
};

IGstAppSink *IGstAppSink::CreateInstance(const char *ElementName)
{
    auto Obj = new FGstAppSinkImpl();
    GST_LOG_DBG_A("GstAppSink: CreateInstance %p %s", Obj, ElementName);
    return Obj;
}

void FGstAppSinkImpl::Destroy()
{
    GST_LOG_DBG_A("GstAppSink: Destroy %p", this, m_Name.c_str());
    delete this;
}

static GstFlowReturn NewSampleFunc(GstElement *Sink, FGstAppSinkImpl *Context) { return Context->OnNewSample(Sink); }

bool FGstAppSinkImpl::Connect(IGstPipeline *Pipeline, const char *ElementName, IGstAppSinkCallback *Callback)
{
    GST_LOG_DBG_A("GstAppSink: Connect <%s>", ElementName);

    if (m_AppSink)
    {
        GST_LOG_ERR_A("GstAppSink: Already connected");
        return false;
    }

    for (;;)
    {
        m_Name = ElementName;
        m_Callback = Callback;

        m_AppSink = gst_bin_get_by_name(GST_BIN(Pipeline->GetGPipeline()), ElementName);
        if (!m_AppSink)
        {
            GST_LOG_ERR_A("gst_bin_get_by_name failed");
            break;
        }

        GstCaps *caps = gst_app_sink_get_caps(GST_APP_SINK(m_AppSink));
        guint num_caps = gst_caps_get_size(caps);
        if (num_caps > 0)
        {
            gchar *format;
            gint fps_n = 0, fps_d;
            GstStructure *st = gst_caps_get_structure(caps, 0);
            const char *st_name = gst_structure_get_name(st);
            gboolean klv_parsed;

            if (gst_structure_get(st,
                                  "width", G_TYPE_INT, &m_Width,
                                  "height", G_TYPE_INT, &m_Height,
                                  "format", G_TYPE_STRING, &format,
                                  "framerate", GST_TYPE_FRACTION, &fps_n, &fps_d,
                                  NULL))
            {
                m_Framerate = fps_n / fps_d;
                GST_LOG_DBG_A("GstAppSink: Found CAPS:%s width:%i height:%i format:%s fps:%i", st_name, m_Width, m_Height, format, m_Framerate);
                if (strncmp(format, "BGRx", 4) == 0)
                {
                    m_Format = GST_VIDEO_FORMAT_BGRx;
                }
                g_free(format);
            }
            else if (g_str_equal(st_name, "meta/x-klv") && gst_structure_get(st,
                                                                             "parsed", G_TYPE_BOOLEAN, &klv_parsed,
                                                                             "framerate", GST_TYPE_FRACTION, &fps_n, &fps_d,
                                                                             NULL))
            {
                m_Framerate = fps_n / fps_d;
                m_IsKlv = true;
                GST_LOG_DBG_A("GstAppSink: Found CAPS:%s parsed:%i fps:%i", st_name, klv_parsed, m_Framerate);
            }
        }
        gst_caps_unref(caps);

        g_object_set(m_AppSink, "emit-signals", TRUE, nullptr);
        g_signal_connect(m_AppSink, "new-sample", G_CALLBACK(NewSampleFunc), this);

        GST_LOG_DBG_A("GstAppSink: Connect SUCCESS");
        return true;
    }

    GST_LOG_ERR_A("GstAppSink: Connect FAILED");
    Disconnect();
    return false;
}

void FGstAppSinkImpl::Disconnect()
{
    if (m_AppSink)
    {
        GST_LOG_DBG_A("GstAppSink: Disconnect <%s>", m_Name.c_str());

        g_object_set(m_AppSink, "emit-signals", FALSE, nullptr);
        gst_object_unref(m_AppSink);
        m_AppSink = nullptr;
    }

    DeallocSamplePool();

    m_Format = GST_VIDEO_FORMAT_UNKNOWN;
    m_Width = 0;
    m_Height = 0;
}

GstFlowReturn FGstAppSinkImpl::OnNewSample(GstElement *Sink)
{
    if (!m_Callback)
        return GST_FLOW_OK;

    SCOPED_PROFILER;

    GstSample *Sample = nullptr;
    g_signal_emit_by_name(Sink, "pull-sample", &Sample);

    if (Sample)
    {
        if (m_IsKlv)
        {
        }
        else if (m_Format == GST_VIDEO_FORMAT_UNKNOWN)
        {
            GstCaps *Caps = gst_sample_get_caps(Sample);
            GstStructure *CapsStruct = gst_caps_get_structure(Caps, 0);

            const gchar *FormatStr = gst_structure_get_string(CapsStruct, "format");
            m_Format = gst_video_format_from_string(FormatStr);

            gst_structure_get_int(CapsStruct, "width", &m_Width);
            gst_structure_get_int(CapsStruct, "height", &m_Height);
        }

        if (m_IsKlv)
        {
            GST_LOG_DBG_A("GstAppSink: <%s> OnKlvSample", m_Name.c_str());

            IGstSample *SampleObj = AllocSample();
            SampleObj->Init(Sample);

            if (SampleObj->Map())
            {
                m_Callback->CbGstKlvSampleReceived(SampleObj);
                SampleObj->Unmap();
            }
            ReleaseSample(SampleObj);
        }
        else if (m_Format != GST_VIDEO_FORMAT_UNKNOWN)
        {
            IGstSample *SampleObj = AllocSample();
            SampleObj->Init(Sample, m_Format, m_Width, m_Height);

            if (SampleObj->Map())
            {
                m_Callback->CbGstTextureSampleReceived(SampleObj);
            }
            else
            {
                ReleaseSample(SampleObj);
            }
        }
        else
        {
            gst_sample_unref(Sample);
        }
    }

    return GST_FLOW_OK;
}

void FGstAppSinkImpl::ReleaseSample(IGstSample *Sample)
{
    SCOPED_PROFILER;

    if (Sample)
    {
        Sample->Release();
        DeallocSample(Sample);
    }
}

IGstSample *FGstAppSinkImpl::AllocSample()
{
    SCOPED_PROFILER;

    if (!m_SamplePool.empty())
    {
        std::lock_guard<std::mutex> guard(m_SampleMx);
        if (!m_SamplePool.empty())
        {
            auto Sample = m_SamplePool.back();
            m_SamplePool.pop_back();
            return Sample;
        }
    }

    auto Sample = IGstSample::CreateInstance(nullptr);
    return Sample;
}

void FGstAppSinkImpl::DeallocSample(IGstSample *Sample)
{
    SCOPED_PROFILER;

    std::lock_guard<std::mutex> guard(m_SampleMx);
    m_SamplePool.push_back(Sample);
}

void FGstAppSinkImpl::DeallocSamplePool()
{
    std::lock_guard<std::mutex> guard(m_SampleMx);
    for (auto *Sample : m_SamplePool)
    {
        Sample->Destroy();
    }
    m_SamplePool.clear();
}
