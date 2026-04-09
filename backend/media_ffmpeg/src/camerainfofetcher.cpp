#include <fplayer/backend/media_ffmpeg/camerainfofetcher.h>

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <logger/logger.h>

#include <mutex>
#include <cstdlib>
#include <QSet>

#ifdef _WIN32
#include <dshow.h>
#endif


// 可选：为线程安全做准备
static std::mutex log_mutex;

static void loggerCallback(void* ptr, int level, const char* fmt, va_list vargs)
{
	// 过滤等级（关键）
	if (level > av_log_get_level())
	{
		return;
	}
	static char message[2048];
	std::lock_guard<std::mutex> lock(log_mutex);

	vsnprintf(message, sizeof(message), fmt, vargs);

	// 可选：过滤掉结尾的换行
	std::string str(message);
	if (!str.empty() && str.back() == '\n')
	{
		str.pop_back();
	}

	// 将 FFmpeg 的日志级别转换为 Logger 自己的日志级别
	if (level <= AV_LOG_PANIC || level == AV_LOG_FATAL)
	{
		LOG_CRITI("[ffmpeg]", str);
	}
	else if (level <= AV_LOG_ERROR)
	{
		LOG_ERROR("[ffmpeg]", str);
	}
	else if (level <= AV_LOG_WARNING)
	{
		LOG_WARN("[ffmpeg]", str);
	}
	else if (level <= AV_LOG_INFO)
	{
		LOG_INFO("[ffmpeg]", str);
	}
	else if (level <= AV_LOG_VERBOSE)
	{
		LOG_DEBUG("[ffmpeg]", str);
	}
	else if (level <= AV_LOG_DEBUG)
	{
		LOG_DEBUG("[ffmpeg]", str);
	}
	else
	{
		LOG_TRACE("[ffmpeg]", str);
	}
}

QVector<QList<fplayer::CameraDescriptionFetcher::FCameraFormat>> fplayer::CameraDescriptionFetcher::m_cameraFormats;

namespace
{
}

fplayer::CameraDescriptionFetcher::CameraDescriptionFetcher()
{
	av_log_set_level(AV_LOG_FATAL);
	av_log_set_callback(loggerCallback);// 指定ffmpeg日志输出到logger
}

fplayer::CameraDescriptionFetcher& fplayer::CameraDescriptionFetcher::instance()
{
	// 利用函数内置变量创建单例
	static CameraDescriptionFetcher instance;
	return instance;
}

QList<fplayer::CameraDescription> fplayer::CameraDescriptionFetcher::getDescriptions()
{
	// 每次刷新先清空并重建，保证 UI 获取的是“当前系统快照”。
	m_cameraFormats.clear();
	QList<fplayer::CameraDescription> descriptions;

	// 设备枚举走平台 API（DirectShow）：
	// FFmpeg 更擅长“打开与解码”，不擅长完整设备能力枚举。
	avdevice_register_all();
#ifdef _WIN32 // 处理Windows平台，DirectShow

	// 1）初始化COM
	// DirectShow 是 COM组件，所以必须初始化 COM
	HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);// para1：当前线程；para2：多线程COM模型
	// 检测COM初始化是否成功，以及COM是否设置上线程模式
	if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE)
	{
		return descriptions;
	}

	// 2）创建摄像头设备的设备枚举器
	// 创建设备枚举器，枚举多媒体设备
	ICreateDevEnum* devEnum = nullptr;
	// 用来承接设备的枚举结果（成功后得到 IEnumMoniker*，用于遍历所有摄像头设备的“moniker”（设备标识对象）
	IEnumMoniker* enumMoniker = nullptr;

	HRESULT hr = CoCreateInstance(// 创建系统设备枚举器对象
			CLSID_SystemDeviceEnum,// 要创建的 COM 类的 CLSID
			// 这是一个预定义的 COM 类标识符（CLSID），代表“系统设备枚举器”。它是 DirectShow 提供的一个工厂类，用于创建各类设备的枚举器（比如摄像头、音频设备等）。
			nullptr,// 不使用聚合（aggregation）
			CLSCTX_INPROC_SERVER,// 在当前进程中加载 DLL（in-process server）
			// 表示该 COM 对象实现在一个 DLL 中，并在当前进程内运行（而不是跨进程或远程）。DirectShow 的设备枚举器通常以内存 DLL 形式存在（如 quartz.dll）。
			IID_ICreateDevEnum,// 请求的接口 ID
			// 我们希望从这个 COM 对象获取 ICreateDevEnum 接口。该接口的作用是：为特定类型的设备创建枚举器（enumerator）。
			(void**)&devEnum);// 输出：指向 ICreateDevEnum 接口的指针，成功后，devEnum 将指向一个有效的 ICreateDevEnum 接口实例。

	if (FAILED(hr))
	{
		return descriptions;
	}

	hr = devEnum->CreateClassEnumerator(// 创建“视频输入设备”的枚举器
			CLSID_VideoInputDeviceCategory,// 设备类别：视频输入设备（即摄像头）
			// 这是一个特殊的 CLSID，代表“视频捕捉设备类别”，也就是所有可用的摄像头（包括 USB 摄像头、集成摄像头、虚拟摄像头等）。
			&enumMoniker,// 输出：指向 IEnumMoniker 的指针
			// 成功后，enumMoniker 是一个枚举器对象，可以用来遍历系统中所有属于该类别的设备。每个设备由一个 IMoniker（“moniker” 即设备标识符）表示。
			0);// 保留参数，必须为 0

	// 注意这里不是用 FAILED(hr)，而是直接判断是否等于 S_OK。这是因为：
	// 如果系统中没有摄像头，CreateClassEnumerator 会返回 S_FALSE（不是错误，但表示“无设备”）。
	// 所以只有 S_OK 才表示成功且有设备可枚举。
	// 其他值（如 S_FALSE、E_FAIL 等）都视为“无有效摄像头”，释放资源后返回空列表。
	if (hr != S_OK)
	{
		// 资源清理
		devEnum->Release();// 释放之前创建的 ICreateDevEnum 接口
		return descriptions;// 返回空列表
	}

	// 3）遍历所有摄像头设备
	IMoniker* moniker = nullptr;

	while (enumMoniker->Next(1, &moniker, nullptr) == S_OK)
	{
		CameraDescription device;
		// 去重集合：某些驱动会重复上报同一格式。
		QSet<QString> seenFormats;

		QList<FCameraFormat> formatList;

		// 读取 FriendlyName 摄像头名称
		IPropertyBag* propBag = nullptr;
		hr = moniker->BindToStorage(
				nullptr,
				nullptr,
				IID_IPropertyBag,
				(void**)&propBag);

		if (SUCCEEDED(hr))
		{
			VARIANT var;
			VariantInit(&var);

			if (SUCCEEDED(propBag->Read(L"FriendlyName", &var, 0)))
			{
				device.description = QString::fromWCharArray(var.bstrVal);
			}

			VariantClear(&var);
			propBag->Release();
		}

		// 获取设备ID
		LPOLESTR displayName = nullptr;
		moniker->GetDisplayName(nullptr, nullptr, &displayName);

		if (displayName)
		{
			device.id = QString::fromWCharArray(displayName);
			CoTaskMemFree(displayName);
		}

		// 绑定 Filter
		IBaseFilter* filter = nullptr;
		hr = moniker->BindToObject(
				nullptr,
				nullptr,
				IID_IBaseFilter,
				(void**)&filter);

		if (SUCCEEDED(hr))
		{
			IEnumPins* enumPins = nullptr;
			filter->EnumPins(&enumPins);

			IPin* pin = nullptr;

			while (enumPins->Next(1, &pin, nullptr) == S_OK)
			{
				PIN_DIRECTION dir;
				pin->QueryDirection(&dir);

				if (dir == PINDIR_OUTPUT)
				{
					IAMStreamConfig* streamConfig = nullptr;

					if (SUCCEEDED(pin->QueryInterface(
						IID_IAMStreamConfig,
						(void**)&streamConfig)))
					{
						int count = 0;
						int size = 0;

						streamConfig->GetNumberOfCapabilities(
								&count,
								&size);

						BYTE* caps = new BYTE[size];

						for (int i = 0; i < count; i++)
						{
							AM_MEDIA_TYPE* mediaType = nullptr;

							if (SUCCEEDED(streamConfig->GetStreamCaps(
								i,
								&mediaType,
								caps)))
							{
								if (mediaType->formattype == FORMAT_VideoInfo)
								{
									VIDEOINFOHEADER* vih =
											(VIDEOINFOHEADER*)mediaType->pbFormat;

									int width = std::abs(vih->bmiHeader.biWidth);
									int height = std::abs(vih->bmiHeader.biHeight);

									int fps = 0;

									if (vih->AvgTimePerFrame > 0)
									{
										fps = 10000000 /
										      vih->AvgTimePerFrame;
									}

									if (fps >= 1 && fps <= 240)
									{
										QString formatText = QString("%1x%2 %3fps")
										                     .arg(width)
										                     .arg(height)
										                     .arg(fps);
										if (seenFormats.contains(formatText))
										{
											continue;
										}
										seenFormats.insert(formatText);

										FCameraFormat fmt;
										fmt.width = width;
										fmt.height = height;
										fmt.fps = fps;

										// 策略说明：
										// 启动阶段仅做“候选格式枚举”，不做开流验证。
										// 这样可以避免启动慢、摄像头反复亮灯；
										// 真正验证在 selectCamera/selectCameraFormat 时执行。
										formatList.push_back(fmt);
										device.formats.push_back(formatText);
									}
								}

								// 释放 mediaType
								if (mediaType)
								{
									if (mediaType->cbFormat != 0)
									{
										CoTaskMemFree(mediaType->pbFormat);
										mediaType->pbFormat = nullptr;
									}

									if (mediaType->pUnk)
									{
										mediaType->pUnk->Release();
										mediaType->pUnk = nullptr;
									}

									CoTaskMemFree(mediaType);
								}
							}
						}

						delete[] caps;
						streamConfig->Release();
					}
				}

				pin->Release();
			}

			enumPins->Release();
			filter->Release();
		}

		descriptions.push_back(device);
		m_cameraFormats.push_back(formatList);

		moniker->Release();
	}

#endif
	return descriptions;
}