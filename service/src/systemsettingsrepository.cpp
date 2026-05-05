#include <fplayer/service/systemsettingsrepository.h>

#include <yamltool/yamltool.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace
{
QStringList loadStringList(const YamlTool::YamlNode& root, const std::string& key)
{
	QStringList list;
	const YamlTool::YamlNode seq = YamlTool::YamlTool::getNode(root, key);
	if (!seq.isSequence())
	{
		return list;
	}
	for (size_t i = 0; i < seq.size(); ++i)
	{
		const YamlTool::YamlNode item = YamlTool::YamlTool::getSequenceNode(seq, i);
		const std::string value = YamlTool::YamlTool::getDef<std::string>(item, "__value__", "");
		if (!value.empty())
		{
			list << QString::fromStdString(value);
		}
	}
	return list;
}

YamlTool::YamlNode makeScalarNode(const std::string& value)
{
	YamlTool::YamlNode n;
	YamlTool::YamlTool::set<std::string>(n, "__value__", value);
	return n;
}

void saveStringList(YamlTool::YamlNode& root, const std::string& key, const QStringList& list)
{
	YamlTool::YamlNode seq;
	for (const QString& item : list)
	{
		YamlTool::YamlTool::pushBack(seq, makeScalarNode(item.toStdString()));
	}
	YamlTool::YamlTool::addNode(root, key, seq);
}
}

fplayer::SystemSettingsRepository::SystemSettingsRepository(QString filePath) : m_filePath(std::move(filePath))
{
}

bool fplayer::SystemSettingsRepository::load(fplayer::SystemSettings& data) const
{
	try
	{
		if (m_filePath.isEmpty() || !QFile::exists(m_filePath))
		{
			return false;
		}
		YamlTool::YamlNode root;
		if (!YamlTool::YamlTool::loadFile(root, m_filePath.toStdString()))
		{
			return false;
		}
		const auto loadString = [&root](const std::string& key, const QString& fallback = QString()) {
			const std::string v = YamlTool::YamlTool::getDef<std::string>(root, key, fallback.toStdString());
			return QString::fromStdString(v);
		};
		data.screenshotDir = loadString("screenshot_dir", data.screenshotDir);
		data.recordDir = loadString("record_dir", data.recordDir);
		data.pushGateway = loadString("push_gateway", data.pushGateway);
		data.pushServiceApp = loadString("push_service_app", data.pushServiceApp);
		data.pushServiceStream = loadString("push_service_stream", data.pushServiceStream);
		data.pullGateway = loadString("pull_gateway", data.pullGateway);
		data.pullServiceApp = loadString("pull_service_app", data.pullServiceApp);
		data.pullServiceStream = loadString("pull_service_stream", data.pullServiceStream);
		data.pushRouteMode = loadString("push_route_mode", data.pushRouteMode);
		data.pushServiceMode = loadString("push_service_mode", data.pushServiceMode);
		data.pushProtocolTemplate = loadString("push_protocol_template", data.pushProtocolTemplate);
		data.pushFps = YamlTool::YamlTool::getDef<int>(root, "push_fps", data.pushFps);
		data.pushSize = loadString("push_size", data.pushSize);
		data.pushBitrateKbps = YamlTool::YamlTool::getDef<int>(root, "push_bitrate_kbps", data.pushBitrateKbps);
		data.pushEncoder = loadString("push_encoder", data.pushEncoder);
		data.pushAudioInput = loadString("push_audio_input", data.pushAudioInput);
		data.pushAudioOutput = loadString("push_audio_output", data.pushAudioOutput);
		data.pushKeepAspect = YamlTool::YamlTool::getDef<int>(root, "push_keep_aspect", data.pushKeepAspect ? 1 : 0) != 0;
		data.composeOutputSize = loadString("compose_output_size", data.composeOutputSize);
		data.screenCaptureBackend = loadString("screen_capture_backend", data.screenCaptureBackend);
		data.closeToTrayOnClose = YamlTool::YamlTool::getDef<int>(root, "close_to_tray_on_close", data.closeToTrayOnClose ? 1 : 0) != 0;
		data.aiEndpoint = loadString("ai_endpoint", data.aiEndpoint);
		data.aiApiKey = loadString("ai_api_key", data.aiApiKey);
		data.aiModel = loadString("ai_model", data.aiModel);
		data.aiUserBubbleColor = loadString("ai_user_bubble_color", data.aiUserBubbleColor);
		data.aiAiBubbleColor = loadString("ai_ai_bubble_color", data.aiAiBubbleColor);
		data.aiChatBgColor = loadString("ai_chat_bg_color", data.aiChatBgColor);
		data.imagePoolToolbarColor = loadString("image_pool_toolbar_color", data.imagePoolToolbarColor);
		data.aiFontFamily = loadString("ai_font_family", data.aiFontFamily);
		data.aiFontSize = YamlTool::YamlTool::getDef<int>(root, "ai_font_size", data.aiFontSize);
		data.aiTextColor = loadString("ai_text_color", data.aiTextColor);
		data.userTextColor = loadString("user_text_color", data.userTextColor);
			data.aiSystemBubbleColor = loadString("ai_system_bubble_color", data.aiSystemBubbleColor);
			data.aiSystemTextColor = loadString("ai_system_text_color", data.aiSystemTextColor);
			data.aiSenderColor = loadString("ai_sender_color", data.aiSenderColor);
			data.aiSystemSenderColor = loadString("ai_system_sender_color", data.aiSystemSenderColor);
			data.theme = YamlTool::YamlTool::getDef<int>(root, "theme", data.theme);
		data.recentPushInputs = loadStringList(root, "recent_push_inputs");
		data.recentPushOutputs = loadStringList(root, "recent_push_outputs");
		data.recentPullInputs = loadStringList(root, "recent_pull_inputs");
		data.recentPullOutputs = loadStringList(root, "recent_pull_outputs");
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool fplayer::SystemSettingsRepository::save(const fplayer::SystemSettings& data) const
{
	if (m_filePath.isEmpty())
	{
		return false;
	}
	QFileInfo info(m_filePath);
	QDir dir(info.absolutePath());
	if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
	{
		return false;
	}
	YamlTool::YamlNode root;
	YamlTool::YamlTool::set<std::string>(root, "screenshot_dir", data.screenshotDir.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "record_dir", data.recordDir.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "push_gateway", data.pushGateway.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "push_service_app", data.pushServiceApp.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "push_service_stream", data.pushServiceStream.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "pull_gateway", data.pullGateway.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "pull_service_app", data.pullServiceApp.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "pull_service_stream", data.pullServiceStream.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "push_route_mode", data.pushRouteMode.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "push_service_mode", data.pushServiceMode.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "push_protocol_template", data.pushProtocolTemplate.toStdString());
	YamlTool::YamlTool::set<int>(root, "push_fps", data.pushFps);
	YamlTool::YamlTool::set<std::string>(root, "push_size", data.pushSize.toStdString());
	YamlTool::YamlTool::set<int>(root, "push_bitrate_kbps", data.pushBitrateKbps);
	YamlTool::YamlTool::set<std::string>(root, "push_encoder", data.pushEncoder.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "push_audio_input", data.pushAudioInput.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "push_audio_output", data.pushAudioOutput.toStdString());
	YamlTool::YamlTool::set<int>(root, "push_keep_aspect", data.pushKeepAspect ? 1 : 0);
	YamlTool::YamlTool::set<std::string>(root, "compose_output_size", data.composeOutputSize.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "screen_capture_backend", data.screenCaptureBackend.toStdString());
	YamlTool::YamlTool::set<int>(root, "close_to_tray_on_close", data.closeToTrayOnClose ? 1 : 0);
	YamlTool::YamlTool::set<std::string>(root, "ai_endpoint", data.aiEndpoint.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "ai_api_key", data.aiApiKey.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "ai_model", data.aiModel.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "ai_user_bubble_color", data.aiUserBubbleColor.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "ai_ai_bubble_color", data.aiAiBubbleColor.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "ai_chat_bg_color", data.aiChatBgColor.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "image_pool_toolbar_color", data.imagePoolToolbarColor.toStdString());
	YamlTool::YamlTool::set<std::string>(root, "ai_font_family", data.aiFontFamily.toStdString());
	YamlTool::YamlTool::set<int>(root, "ai_font_size", data.aiFontSize);
		YamlTool::YamlTool::set<std::string>(root, "user_text_color", data.userTextColor.toStdString());
		YamlTool::YamlTool::set<std::string>(root, "ai_system_bubble_color", data.aiSystemBubbleColor.toStdString());
		YamlTool::YamlTool::set<std::string>(root, "ai_system_text_color", data.aiSystemTextColor.toStdString());
		YamlTool::YamlTool::set<std::string>(root, "ai_sender_color", data.aiSenderColor.toStdString());
		YamlTool::YamlTool::set<std::string>(root, "ai_system_sender_color", data.aiSystemSenderColor.toStdString());
		YamlTool::YamlTool::set<int>(root, "theme", data.theme);
		YamlTool::YamlTool::set<std::string>(root, "ai_text_color", data.aiTextColor.toStdString());
	saveStringList(root, "recent_push_inputs", data.recentPushInputs);
	saveStringList(root, "recent_push_outputs", data.recentPushOutputs);
	saveStringList(root, "recent_pull_inputs", data.recentPullInputs);
	saveStringList(root, "recent_pull_outputs", data.recentPullOutputs);
	YamlTool::YamlTool::saveAsFile(root, m_filePath.toStdString());
	return true;
}

void fplayer::SystemSettingsRepository::addRecent(QStringList& list, const QString& value, const int maxItems)
{
	const QString v = value.trimmed();
	if (v.isEmpty())
	{
		return;
	}
	list.removeAll(v);
	list.prepend(v);
	while (list.size() > qMax(1, maxItems))
	{
		list.removeLast();
	}
}

QString fplayer::SystemSettingsRepository::filePath() const
{
	return m_filePath;
}
