#include "OBSBasicStatusBar.hpp"
#include "ui_StatusBarWidget.h"

#include <widgets/OBSBasic.hpp>

#include <algorithm>
#include <cstring>

#include <QStringList>

#include "moc_OBSBasicStatusBar.cpp"

static constexpr int bitrateUpdateSeconds = 2;
static constexpr int congestionUpdateSeconds = 4;
static constexpr float excellentThreshold = 0.0f;
static constexpr float goodThreshold = 0.3333f;
static constexpr float mediocreThreshold = 0.6667f;
static constexpr float badThreshold = 1.0f;
static constexpr char aitumOutputPrefix[] = "Aitum Stream Suite Output ";

struct StreamOutputStatus {
	std::string name;
	QString displayName;
	uint64_t totalBytes;
	int droppedFrames;
	int totalFrames;
	float congestion;
};

static StreamOutputStatus GetStreamOutputStatus(obs_output_t *output, const QString &displayName)
{
	return {obs_output_get_name(output),         displayName,
		obs_output_get_total_bytes(output),  obs_output_get_frames_dropped(output),
		obs_output_get_total_frames(output), obs_output_get_congestion(output)};
}

static bool IsActiveStreamOutput(obs_output_t *output)
{
	return obs_output_active(output) || obs_output_reconnecting(output);
}

static std::vector<StreamOutputStatus> GetActiveStreamOutputs(obs_output_t *builtInOutput)
{
	std::vector<StreamOutputStatus> outputs;
	if (builtInOutput && IsActiveStreamOutput(builtInOutput)) {
		outputs.emplace_back(GetStreamOutputStatus(builtInOutput, QStringLiteral("Built-in stream")));
	}

	struct EnumContext {
		obs_output_t *builtInOutput;
		std::vector<StreamOutputStatus> *outputs;
	} context{builtInOutput, &outputs};

	obs_enum_outputs(
		[](void *data, obs_output_t *output) {
			auto *context = static_cast<EnumContext *>(data);
			if (output == context->builtInOutput || !IsActiveStreamOutput(output)) {
				return true;
			}

			const char *name = obs_output_get_name(output);
			if (!name || strncmp(name, aitumOutputPrefix, sizeof(aitumOutputPrefix) - 1) != 0) {
				return true;
			}

			const char *outputId = obs_output_get_id(output);
			if (!obs_output_get_service(output) && (!outputId || strcmp(outputId, "ffmpeg_output") != 0)) {
				return true; // Aitum recordings use the same name prefix, so only service and network FFmpeg outputs belong in stream status.
			}

			QString displayName = QString::fromUtf8(name + sizeof(aitumOutputPrefix) - 1);
			context->outputs->emplace_back(GetStreamOutputStatus(output, displayName));
			return true;
		},
		&context);

	return outputs;
}

OBSBasicStatusBar::OBSBasicStatusBar(QWidget *parent)
	: QStatusBar(parent),
	  excellentPixmap(QIcon(":/res/images/network-excellent.svg").pixmap(QSize(16, 16))),
	  goodPixmap(QIcon(":/res/images/network-good.svg").pixmap(QSize(16, 16))),
	  mediocrePixmap(QIcon(":/res/images/network-mediocre.svg").pixmap(QSize(16, 16))),
	  badPixmap(QIcon(":/res/images/network-bad.svg").pixmap(QSize(16, 16))),
	  recordingActivePixmap(QIcon(":/res/images/recording-active.svg").pixmap(QSize(16, 16))),
	  recordingPausePixmap(QIcon(":/res/images/recording-pause.svg").pixmap(QSize(16, 16))),
	  streamingActivePixmap(QIcon(":/res/images/streaming-active.svg").pixmap(QSize(16, 16)))
{
	congestionArray.reserve(congestionUpdateSeconds);

	statusWidget = new StatusBarWidget(this);
	statusWidget->ui->delayInfo->setText("");
	statusWidget->ui->droppedFrames->setText(QTStr("DroppedFrames").arg("0", "0.0"));
	statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
	statusWidget->ui->streamIcon->setPixmap(streamingInactivePixmap);
	statusWidget->ui->streamTime->setDisabled(true);
	statusWidget->ui->recordIcon->setPixmap(recordingInactivePixmap);
	statusWidget->ui->recordTime->setDisabled(true);
	statusWidget->ui->cpuUsage->setToolTip(QStringLiteral("Whole OBS++ process, including Aitum++."));
	statusWidget->ui->delayFrame->hide();
	statusWidget->ui->issuesFrame->hide();
	statusWidget->ui->kbps->hide();

	addPermanentWidget(statusWidget, 1);
	setMinimumHeight(statusWidget->height());

	UpdateIcons();
	connect(App(), &OBSApp::StyleChanged, this, &OBSBasicStatusBar::UpdateIcons);

	messageTimer = new QTimer(this);
	messageTimer->setSingleShot(true);
	connect(messageTimer, &QTimer::timeout, this, &OBSBasicStatusBar::clearMessage);

	clearMessage();
}

void OBSBasicStatusBar::Activate()
{
	if (!active) {
		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, &OBSBasicStatusBar::UpdateStatusBar);

		int skipped = video_output_get_skipped_frames(obs_get_video());
		int total = video_output_get_total_frames(obs_get_video());

		totalStreamSeconds = 0;
		totalRecordSeconds = 0;
		lastSkippedFrameCount = 0;
		startSkippedFrameCount = skipped;
		startTotalFrameCount = total;

		refreshTimer->start(1000);
		active = true;

		if (streamOutput) {
			statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
		}
	}

	if (streamOutput) {
		statusWidget->ui->streamIcon->setPixmap(streamingActivePixmap);
		statusWidget->ui->streamTime->setDisabled(false);
		statusWidget->ui->issuesFrame->show();
		statusWidget->ui->kbps->show();
		firstCongestionUpdate = true;
	}

	if (recordOutput) {
		statusWidget->ui->recordIcon->setPixmap(recordingActivePixmap);
		statusWidget->ui->recordTime->setDisabled(false);
	}
}

void OBSBasicStatusBar::Deactivate()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main) {
		return;
	}

	if (!streamOutput) {
		statusWidget->ui->streamTime->setText(QString("00:00:00"));
		statusWidget->ui->streamTime->setDisabled(true);
		statusWidget->ui->streamIcon->setPixmap(streamingInactivePixmap);
		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
		statusWidget->ui->delayFrame->hide();
		statusWidget->ui->issuesFrame->hide();
		statusWidget->ui->kbps->hide();
		totalStreamSeconds = 0;
		congestionArray.clear();
		disconnected = false;
		firstCongestionUpdate = false;
	}

	if (!recordOutput) {
		statusWidget->ui->recordTime->setText(QString("00:00:00"));
		statusWidget->ui->recordTime->setDisabled(true);
		statusWidget->ui->recordIcon->setPixmap(recordingInactivePixmap);
		totalRecordSeconds = 0;
	}

	if (main->outputHandler && !main->outputHandler->Active()) {
		delete refreshTimer;

		statusWidget->ui->delayInfo->setText("");
		statusWidget->ui->droppedFrames->setText(QTStr("DroppedFrames").arg("0", "0.0"));
		statusWidget->ui->kbps->setText("0 kbps");

		delaySecTotal = 0;
		delaySecStarting = 0;
		delaySecStopping = 0;
		reconnectTimeout = 0;
		active = false;
		overloadedNotify = true;

		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
	}
}

void OBSBasicStatusBar::UpdateDelayMsg()
{
	QString msg;

	if (delaySecTotal) {
		if (delaySecStarting && !delaySecStopping) {
			msg = QTStr("Basic.StatusBar.DelayStartingIn");
			msg = msg.arg(QString::number(delaySecStarting));

		} else if (!delaySecStarting && delaySecStopping) {
			msg = QTStr("Basic.StatusBar.DelayStoppingIn");
			msg = msg.arg(QString::number(delaySecStopping));

		} else if (delaySecStarting && delaySecStopping) {
			msg = QTStr("Basic.StatusBar.DelayStartingStoppingIn");
			msg = msg.arg(QString::number(delaySecStopping), QString::number(delaySecStarting));
		} else {
			msg = QTStr("Basic.StatusBar.Delay");
			msg = msg.arg(QString::number(delaySecTotal));
		}

		if (!statusWidget->ui->delayFrame->isVisible()) {
			statusWidget->ui->delayFrame->show();
		}

		statusWidget->ui->delayInfo->setText(msg);
	}
}

void OBSBasicStatusBar::UpdateBandwidth()
{
	OBSOutput output = OBSGetStrongRef(streamOutput);
	const std::vector<StreamOutputStatus> outputs = GetActiveStreamOutputs(output);
	if (outputs.empty()) {
		lastBytesSentByOutput.clear();
		lastBytesSentTime = 0;
		statusWidget->ui->kbps->setText(QStringLiteral("0 kbps"));
		statusWidget->ui->kbps->setToolTip(QString());
		statusWidget->ui->kbps->hide();
		return;
	}

	if (!statusWidget->ui->kbps->isVisible()) {
		statusWidget->ui->kbps->show();
	}

	const uint64_t bytesSentTime = os_gettime_ns();
	if (!lastBytesSentTime) {
		for (const StreamOutputStatus &stream : outputs) {
			lastBytesSentByOutput.emplace(stream.name, stream.totalBytes);
		}
		lastBytesSentTime = bytesSentTime;
		return;
	}
	for (const StreamOutputStatus &stream : outputs) {
		lastBytesSentByOutput.try_emplace(stream.name, stream.totalBytes);
	}

	const double timePassed = double(bytesSentTime - lastBytesSentTime) / 1000000000.0;
	if (timePassed < bitrateUpdateSeconds) {
		return;
	}

	QStringList bitrateParts;
	QStringList tooltipParts;
	std::map<std::string, uint64_t> currentBytesSentByOutput;
	for (const StreamOutputStatus &stream : outputs) {
		const auto previous = lastBytesSentByOutput.find(stream.name);
		const uint64_t previousBytes = previous == lastBytesSentByOutput.end() ||
							       stream.totalBytes < previous->second
						       ? stream.totalBytes
						       : previous->second;
		const double kbitsPerSec = double((stream.totalBytes - previousBytes) * 8) / timePassed / 1000.0;
		const QString bitrate = QString::number(kbitsPerSec, 'f', 0);
		bitrateParts.emplace_back(bitrate);
		tooltipParts.emplace_back(QStringLiteral("%1: %2 kbps").arg(stream.displayName, bitrate));
		currentBytesSentByOutput.emplace(stream.name, stream.totalBytes);
	}

	statusWidget->ui->kbps->setText(bitrateParts.join(QStringLiteral(" + ")) + QStringLiteral(" kbps"));
	statusWidget->ui->kbps->setToolTip(tooltipParts.join(QLatin1Char('\n')));
	statusWidget->ui->kbps->setMinimumWidth(statusWidget->ui->kbps->width());

	lastBytesSentByOutput = std::move(currentBytesSentByOutput);
	lastBytesSentTime = bytesSentTime;
}

void OBSBasicStatusBar::UpdateCPUUsage()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main) {
		return;
	}

	QString text;
	text += QString("CPU: ") + QString::number(main->GetCPUUsage(), 'f', 1) + QString("%");

	statusWidget->ui->cpuUsage->setText(text);
	statusWidget->ui->cpuUsage->setMinimumWidth(statusWidget->ui->cpuUsage->width());
	if (!active) {
		UpdateBandwidth();
		UpdateDroppedFrames();
	}

	UpdateCurrentFPS();
}

void OBSBasicStatusBar::UpdateCurrentFPS()
{
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	float targetFPS = (float)ovi.fps_num / (float)ovi.fps_den;

	QString text = QString::asprintf("%.2f / %.2f FPS", obs_get_active_fps(), targetFPS);

	statusWidget->ui->fpsCurrent->setText(text);
	statusWidget->ui->fpsCurrent->setMinimumWidth(statusWidget->ui->fpsCurrent->width());
}

void OBSBasicStatusBar::UpdateStreamTime()
{
	totalStreamSeconds++;

	int seconds = totalStreamSeconds % 60;
	int totalMinutes = totalStreamSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	QString text = QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
	statusWidget->ui->streamTime->setText(text);
	if (streamOutput && !statusWidget->ui->streamTime->isEnabled()) {
		statusWidget->ui->streamTime->setDisabled(false);
	}

	if (reconnectTimeout > 0) {
		QString msg = QTStr("Basic.StatusBar.Reconnecting")
				      .arg(QString::number(retries), QString::number(reconnectTimeout));
		showMessage(msg);
		disconnected = true;
		statusWidget->ui->statusIcon->setPixmap(disconnectedPixmap);
		congestionArray.clear();
		reconnectTimeout--;

	} else if (retries > 0) {
		QString msg = QTStr("Basic.StatusBar.AttemptingReconnect");
		showMessage(msg.arg(QString::number(retries)));
	}

	if (delaySecStopping > 0 || delaySecStarting > 0) {
		if (delaySecStopping > 0) {
			--delaySecStopping;
		}
		if (delaySecStarting > 0) {
			--delaySecStarting;
		}
		UpdateDelayMsg();
	}
}

extern volatile bool recording_paused;

void OBSBasicStatusBar::UpdateRecordTime()
{
	bool paused = os_atomic_load_bool(&recording_paused);

	if (!paused) {
		totalRecordSeconds++;

		if (recordOutput && !statusWidget->ui->recordTime->isEnabled()) {
			statusWidget->ui->recordTime->setDisabled(false);
		}
	} else {
		statusWidget->ui->recordIcon->setPixmap(streamPauseIconToggle ? recordingPauseInactivePixmap
									      : recordingPausePixmap);

		streamPauseIconToggle = !streamPauseIconToggle;
	}

	UpdateRecordTimeLabel();
}

void OBSBasicStatusBar::UpdateRecordTimeLabel()
{
	int seconds = totalRecordSeconds % 60;
	int totalMinutes = totalRecordSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	QString text = QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
	if (os_atomic_load_bool(&recording_paused)) {
		text += QStringLiteral(" (PAUSED)");
	}

	statusWidget->ui->recordTime->setText(text);
}

void OBSBasicStatusBar::UpdateDroppedFrames()
{
	OBSOutput output = OBSGetStrongRef(streamOutput);
	const std::vector<StreamOutputStatus> outputs = GetActiveStreamOutputs(output);
	if (outputs.empty()) {
		statusWidget->ui->droppedFrames->setText(QTStr("DroppedFrames").arg("0", "0.0"));
		statusWidget->ui->droppedFrames->setToolTip(QString());
		statusWidget->ui->issuesFrame->hide();
		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
		congestionArray.clear();
		lastCongestion = 0.0f;
		firstCongestionUpdate = false;
		return;
	}

	if (!statusWidget->ui->issuesFrame->isVisible()) {
		statusWidget->ui->issuesFrame->show();
		firstCongestionUpdate = true;
	}

	QStringList droppedParts;
	QStringList tooltipParts;
	float congestion = 0.0f;
	for (const StreamOutputStatus &stream : outputs) {
		const double percent =
			stream.totalFrames ? double(stream.droppedFrames) / double(stream.totalFrames) * 100.0 : 0.0;
		const QString dropped =
			QStringLiteral("%1 (%2%)")
				.arg(QString::number(stream.droppedFrames), QString::number(percent, 'f', 1));
		droppedParts.emplace_back(dropped);
		tooltipParts.emplace_back(QStringLiteral("%1: %2").arg(stream.displayName, dropped));
		congestion = std::max(congestion, stream.congestion);
	}

	QString text = QTStr("DroppedFrames")
			       .arg(QString::number(outputs.front().droppedFrames),
				    QString::number(outputs.front().totalFrames
							    ? double(outputs.front().droppedFrames) /
								      double(outputs.front().totalFrames) * 100.0
							    : 0.0,
						    'f', 1));
	for (qsizetype index = 1; index < droppedParts.size(); ++index) {
		text += QStringLiteral(" + ") + droppedParts.at(index);
	}
	statusWidget->ui->droppedFrames->setText(text);
	statusWidget->ui->droppedFrames->setToolTip(tooltipParts.join(QLatin1Char('\n')));

	/* ----------------------------------- *
	 * calculate congestion color          */

	float avgCongestion = (congestion + lastCongestion) * 0.5f;
	if (avgCongestion < congestion) {
		avgCongestion = congestion;
	}
	if (avgCongestion > 1.0f) {
		avgCongestion = 1.0f;
	}

	lastCongestion = congestion;

	if (disconnected) {
		return;
	}

	bool update = firstCongestionUpdate;
	float congestionOverTime = avgCongestion;

	if (congestionArray.size() >= congestionUpdateSeconds) {
		congestionOverTime = accumulate(congestionArray.begin(), congestionArray.end(), 0.0f) /
				     (float)congestionArray.size();
		congestionArray.clear();
		update = true;
	} else {
		congestionArray.emplace_back(avgCongestion);
	}

	if (update) {
		if (congestionOverTime <= excellentThreshold + EPSILON) {
			statusWidget->ui->statusIcon->setPixmap(excellentPixmap);
		} else if (congestionOverTime <= goodThreshold) {
			statusWidget->ui->statusIcon->setPixmap(goodPixmap);
		} else if (congestionOverTime <= mediocreThreshold) {
			statusWidget->ui->statusIcon->setPixmap(mediocrePixmap);
		} else if (congestionOverTime <= badThreshold) {
			statusWidget->ui->statusIcon->setPixmap(badPixmap);
		}

		firstCongestionUpdate = false;
	}
}

void OBSBasicStatusBar::OBSOutputReconnect(void *data, calldata_t *params)
{
	OBSBasicStatusBar *statusBar = static_cast<OBSBasicStatusBar *>(data);

	int seconds = (int)calldata_int(params, "timeout_sec");
	QMetaObject::invokeMethod(statusBar, &OBSBasicStatusBar::Reconnect, seconds);
}

void OBSBasicStatusBar::OBSOutputReconnectSuccess(void *data, calldata_t *)
{
	OBSBasicStatusBar *statusBar = static_cast<OBSBasicStatusBar *>(data);

	QMetaObject::invokeMethod(statusBar, &OBSBasicStatusBar::ReconnectSuccess);
}

void OBSBasicStatusBar::Reconnect(int seconds)
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());

	if (!retries) {
		main->SysTrayNotify(QTStr("Basic.SystemTray.Message.Reconnecting"), QSystemTrayIcon::Warning);
	}

	reconnectTimeout = seconds;

	if (streamOutput) {
		OBSOutput output = OBSGetStrongRef(streamOutput);
		if (!output) {
			return;
		}

		delaySecTotal = obs_output_get_active_delay(output);
		UpdateDelayMsg();

		retries++;
	}
}

void OBSBasicStatusBar::ReconnectClear()
{
	retries = 0;
	reconnectTimeout = 0;
	lastBytesSentByOutput.clear();
	lastBytesSentTime = 0;
	delaySecTotal = 0;
	UpdateDelayMsg();
}

void OBSBasicStatusBar::ReconnectSuccess()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());

	QString msg = QTStr("Basic.StatusBar.ReconnectSuccessful");
	showMessage(msg, 4000);
	main->SysTrayNotify(msg, QSystemTrayIcon::Information);
	ReconnectClear();

	if (streamOutput) {
		OBSOutput output = OBSGetStrongRef(streamOutput);
		if (!output) {
			return;
		}

		delaySecTotal = obs_output_get_active_delay(output);
		UpdateDelayMsg();
		disconnected = false;
		firstCongestionUpdate = true;
	}
}

void OBSBasicStatusBar::UpdateStatusBar()
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());

	UpdateBandwidth();

	if (streamOutput) {
		UpdateStreamTime();
	}

	if (recordOutput) {
		UpdateRecordTime();
	}

	UpdateDroppedFrames();

	int skipped = video_output_get_skipped_frames(obs_get_video());
	int total = video_output_get_total_frames(obs_get_video());

	skipped -= startSkippedFrameCount;
	total -= startTotalFrameCount;

	int diff = skipped - lastSkippedFrameCount;
	double percentage = double(skipped) / double(total) * 100.0;

	if (diff > 10 && percentage >= 0.1f) {
		showMessage(QTStr("HighResourceUsage"), 4000);
		if (!main->isVisible() && overloadedNotify) {
			main->SysTrayNotify(QTStr("HighResourceUsage"), QSystemTrayIcon::Warning);
			overloadedNotify = false;
		}
	}

	lastSkippedFrameCount = skipped;
}

void OBSBasicStatusBar::StreamDelayStarting(int sec)
{
	OBSBasic *main = qobject_cast<OBSBasic *>(parent());
	if (!main || !main->outputHandler) {
		return;
	}

	OBSOutputAutoRelease output = obs_frontend_get_streaming_output();
	streamOutput = OBSGetWeakRef(output);

	delaySecTotal = delaySecStarting = sec;
	UpdateDelayMsg();
	Activate();
}

void OBSBasicStatusBar::StreamDelayStopping(int sec)
{
	delaySecTotal = delaySecStopping = sec;
	UpdateDelayMsg();
}

void OBSBasicStatusBar::StreamStarted(obs_output_t *output)
{
	streamOutput = OBSGetWeakRef(output);

	streamSigs.emplace_back(obs_output_get_signal_handler(output), "reconnect", OBSOutputReconnect, this);
	streamSigs.emplace_back(obs_output_get_signal_handler(output), "reconnect_success", OBSOutputReconnectSuccess,
				this);

	retries = 0;
	lastBytesSentByOutput.clear();
	lastBytesSentTime = 0;
	Activate();
}

void OBSBasicStatusBar::StreamStopped()
{
	if (streamOutput) {
		streamSigs.clear();

		ReconnectClear();
		streamOutput = nullptr;
		clearMessage();
		Deactivate();
	}
}

void OBSBasicStatusBar::RecordingStarted(obs_output_t *output)
{
	recordOutput = OBSGetWeakRef(output);
	Activate();
}

void OBSBasicStatusBar::RecordingStopped()
{
	recordOutput = nullptr;
	Deactivate();
}

void OBSBasicStatusBar::RecordingPaused()
{
	if (recordOutput) {
		statusWidget->ui->recordIcon->setPixmap(recordingPausePixmap);
		streamPauseIconToggle = true;
	}

	UpdateRecordTimeLabel();
}

void OBSBasicStatusBar::RecordingUnpaused()
{
	if (recordOutput) {
		statusWidget->ui->recordIcon->setPixmap(recordingActivePixmap);
	}

	UpdateRecordTimeLabel();
}

static QPixmap GetPixmap(const QString &filename)
{
	QString path = obs_frontend_is_theme_dark() ? "theme:Dark/" : ":/res/images/";
	return QIcon(path + filename).pixmap(QSize(16, 16));
}

void OBSBasicStatusBar::UpdateIcons()
{
	disconnectedPixmap = GetPixmap("network-disconnected.svg");
	inactivePixmap = GetPixmap("network-inactive.svg");

	streamingInactivePixmap = GetPixmap("streaming-inactive.svg");

	recordingInactivePixmap = GetPixmap("recording-inactive.svg");
	recordingPauseInactivePixmap = GetPixmap("recording-pause-inactive.svg");

	bool streaming = obs_frontend_streaming_active();

	if (!streaming) {
		statusWidget->ui->streamIcon->setPixmap(streamingInactivePixmap);
		statusWidget->ui->statusIcon->setPixmap(inactivePixmap);
	} else {
		if (disconnected) {
			statusWidget->ui->statusIcon->setPixmap(disconnectedPixmap);
		}
	}

	bool recording = obs_frontend_recording_active();

	if (!recording) {
		statusWidget->ui->recordIcon->setPixmap(recordingInactivePixmap);
	}
}

void OBSBasicStatusBar::showMessage(const QString &message, int timeout)
{
	messageTimer->stop();

	statusWidget->ui->message->setText(message);

	if (timeout) {
		messageTimer->start(timeout);
	}
}

void OBSBasicStatusBar::clearMessage()
{
	statusWidget->ui->message->setText("");
}
