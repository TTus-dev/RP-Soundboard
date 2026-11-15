// src/WebRemote.cpp
//----------------------------------
// Small embedded HTTP server so the soundboard can be controlled from a
// phone/tablet/browser on the local network.
//
// Everything runs on the Qt GUI thread (QTcpServer is driven by the event
// loop), so calling into the soundboard here is exactly as safe as clicking a
// button in the main window.
//
// Routes:
//   GET  /                     web UI (src/web/index.html, embedded as Qt resource)
//   GET  /api/state            JSON: config, sounds, playback state, volumes
//   POST /api/play/<id>        play button <id> (0-based) of the active configuration
//   POST /api/stop             stop playback
//   POST /api/pause            toggle pause
//   POST /api/config/<n>       switch configuration (0-based)
//   POST /api/volume?remote=N&local=N   set volume (0-100), either parameter optional
//
// All POST requests must carry an "X-RP" header. Browsers can't send custom
// headers cross-origin without a CORS preflight (which we never allow), so
// this stops random web pages from poking the server via the user's browser.
//----------------------------------

#include "common.h"

#include <algorithm>

#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>

#include "WebRemote.h"
#include "ConfigModel.h"
#include "SoundInfo.h"
#include "samples.h"
#include "main.h"
#include "ts3log.h"

static const int kMaxHeaderBytes = 16 * 1024;
static const int kSocketTimeoutMs = 10000;


WebRemote::WebRemote(ConfigModel* model, QObject* parent) :
	QObject(parent),
	m_model(model),
	m_server(new QTcpServer(this))
{
	connect(m_server, &QTcpServer::newConnection, this, &WebRemote::onNewConnection);

	// Sampler signals may come from another thread, hence queued
	Sampler* sampler = sb_getSampler();
	if (sampler)
	{
		connect(sampler, &Sampler::onStartPlaying, this, &WebRemote::onPlaybackStarted, Qt::QueuedConnection);
		connect(sampler, &Sampler::onStopPlaying, this, &WebRemote::onPlaybackStopped, Qt::QueuedConnection);
	}
}


WebRemote::~WebRemote()
{
	stop();
}


bool WebRemote::start(quint16 port)
{
	if (m_server->isListening())
		return true;

	if (m_indexHtml.isEmpty())
	{
		QFile f(":/web/index.html");
		if (f.open(QIODevice::ReadOnly))
			m_indexHtml = f.readAll();
		else
			logError("WebRemote: could not load embedded web page");
	}

	if (!m_server->listen(QHostAddress::Any, port))
	{
		m_lastError = m_server->errorString();
		logError("WebRemote: could not listen on port %u: %s", (unsigned)port, m_lastError.toUtf8().constData());
		return false;
	}

	m_lastError.clear();
	logInfo("WebRemote: listening on port %u", (unsigned)m_server->serverPort());
	return true;
}


void WebRemote::stop()
{
	if (m_server->isListening())
	{
		m_server->close();
		logInfo("WebRemote: stopped");
	}
	// Drop connections that are still open
	const auto socks = m_server->findChildren<QTcpSocket*>();
	for (QTcpSocket* s : socks)
		s->abort();
}


bool WebRemote::isRunning() const
{
	return m_server->isListening();
}


quint16 WebRemote::port() const
{
	return m_server->isListening() ? m_server->serverPort() : 0;
}


QStringList WebRemote::lanUrls(quint16 port)
{
	QStringList urls;
	const auto ifaces = QNetworkInterface::allInterfaces();
	for (const QNetworkInterface& iface : ifaces)
	{
		const auto flags = iface.flags();
		if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
			(flags & QNetworkInterface::IsLoopBack))
			continue;
		for (const QNetworkAddressEntry& entry : iface.addressEntries())
		{
			const QHostAddress addr = entry.ip();
			if (addr.protocol() != QAbstractSocket::IPv4Protocol)
				continue;
			const quint32 ip = addr.toIPv4Address();
			if ((ip >> 16) == 0xA9FE) // 169.254.x.x link-local, not useful
				continue;
			urls << QString("http://%1:%2/").arg(addr.toString()).arg(port);
		}
	}
	return urls;
}


void WebRemote::onNewConnection()
{
	while (QTcpSocket* sock = m_server->nextPendingConnection())
	{
		connect(sock, &QTcpSocket::readyRead, this, [this, sock]() { onReadyRead(sock); });
		connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
		// Don't let stalled clients hold sockets forever. Timer dies with the socket.
		QTimer::singleShot(kSocketTimeoutMs, sock, [sock]() { sock->abort(); });
	}
}


void WebRemote::onReadyRead(QTcpSocket* sock)
{
	if (sock->property("handled").toBool())
	{
		sock->readAll();
		return;
	}

	QByteArray buf = sock->property("buf").toByteArray();
	buf += sock->readAll();

	const int end = buf.indexOf("\r\n\r\n");
	if (end < 0)
	{
		if (buf.size() > kMaxHeaderBytes)
		{
			sock->setProperty("handled", true);
			respond(sock, jsonError(431, "Request too large"));
			return;
		}
		sock->setProperty("buf", buf);
		return;
	}

	sock->setProperty("handled", true);
	sock->setProperty("buf", QByteArray());

	const QList<QByteArray> lines = buf.left(end).split('\n');
	const QList<QByteArray> reqLine = lines.value(0).trimmed().split(' ');
	if (reqLine.size() < 2)
	{
		respond(sock, jsonError(400, "Bad request"));
		return;
	}

	bool csrfHeader = false;
	for (int i = 1; i < lines.size(); i++)
		if (lines[i].trimmed().toLower().startsWith("x-rp:"))
			csrfHeader = true;

	respond(sock, handleRequest(reqLine[0], reqLine[1], csrfHeader));
}


void WebRemote::respond(QTcpSocket* sock, const Response& r)
{
	const char* reason = "OK";
	switch (r.status)
	{
	case 200: reason = "OK"; break;
	case 204: reason = "No Content"; break;
	case 400: reason = "Bad Request"; break;
	case 403: reason = "Forbidden"; break;
	case 404: reason = "Not Found"; break;
	case 405: reason = "Method Not Allowed"; break;
	case 409: reason = "Conflict"; break;
	case 431: reason = "Request Header Fields Too Large"; break;
	case 500: reason = "Internal Server Error"; break;
	default: break;
	}

	QByteArray out;
	out += "HTTP/1.1 " + QByteArray::number(r.status) + " " + reason + "\r\n";
	out += "Content-Type: " + r.contentType + "\r\n";
	out += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
	out += "Cache-Control: no-store\r\n";
	out += "X-Content-Type-Options: nosniff\r\n";
	out += "Connection: close\r\n\r\n";
	out += r.body;

	sock->write(out);
	sock->disconnectFromHost();
}


WebRemote::Response WebRemote::jsonError(int status, const QString& msg) const
{
	Response r;
	r.status = status;
	QJsonObject o;
	o["ok"] = false;
	o["error"] = msg;
	r.body = QJsonDocument(o).toJson(QJsonDocument::Compact);
	return r;
}


WebRemote::Response WebRemote::jsonOk() const
{
	Response r;
	QJsonObject o;
	o["ok"] = true;
	r.body = QJsonDocument(o).toJson(QJsonDocument::Compact);
	return r;
}


QString WebRemote::soundDisplayName(int id) const
{
	const SoundInfo* info = m_model->getSoundInfo(id);
	if (!info || info->filename.isEmpty())
		return QString();
	if (!info->customText.isEmpty())
	{
		QString t = info->customText;
		return t.replace("\\n", " ").trimmed();
	}
	return QFileInfo(info->filename).baseName();
}


QByteArray WebRemote::stateJson() const
{
	QJsonObject o;
	o["config"] = m_model->getConfiguration();
	o["configs"] = NUM_CONFIGS;
	o["connected"] = sb_isServerConnected();
	o["volumeLocal"] = m_model->getVolumeLocal();
	o["volumeRemote"] = m_model->getVolumeRemote();

	const char* stateName = "silent";
	Sampler* sampler = sb_getSampler();
	if (sampler)
	{
		switch (sampler->getState())
		{
		case Sampler::ePLAYING: stateName = "playing"; break;
		case Sampler::ePAUSED: stateName = "paused"; break;
		default: break;
		}
	}
	o["state"] = stateName;
	o["nowPlaying"] = QFileInfo(m_nowPlayingFile).baseName();
	o["nowPlayingFile"] = QFileInfo(m_nowPlayingFile).fileName();
	o["nowPlayingId"] = m_nowPlayingId;
	o["nowPlayingConfig"] = m_nowPlayingConfig;

	QJsonArray sounds;
	const int count = m_model->getRows() * m_model->getCols();
	for (int i = 0; i < count; i++)
	{
		const SoundInfo* info = m_model->getSoundInfo(i);
		QJsonObject s;
		s["id"] = i;
		const bool empty = !info || info->filename.isEmpty();
		s["empty"] = empty;
		s["name"] = empty ? QString() : soundDisplayName(i);
		s["file"] = empty ? QString() : QFileInfo(info->filename).fileName();
		if (!empty && info->customColorEnabled())
			s["color"] = info->customColor.name(); // "#rrggbb"
		sounds.append(s);
	}
	o["sounds"] = sounds;

	return QJsonDocument(o).toJson(QJsonDocument::Compact);
}


void WebRemote::onPlaybackStarted(bool preview, QString filename)
{
	if (preview)
		return;

	m_nowPlayingFile = filename;
	m_nowPlayingId = -1;
	m_nowPlayingConfig = m_model->getConfiguration();

	auto matches = [&](int id)
	{
		const SoundInfo* info = m_model->getSoundInfo(id);
		return info && info->filename == filename;
	};

	// Several buttons can share one file. Prefer the button that was pressed on the web page,
	// otherwise (desktop button, hotkey) fall back to the first button with that file.
	if (m_pendingPlayId >= 0 && m_pendingConfig == m_nowPlayingConfig && matches(m_pendingPlayId))
		m_nowPlayingId = m_pendingPlayId;
	else
	{
		const int count = m_model->getRows() * m_model->getCols();
		for (int i = 0; i < count; i++)
		{
			if (matches(i))
			{
				m_nowPlayingId = i;
				break;
			}
		}
	}
	m_pendingPlayId = -1;
}


void WebRemote::onPlaybackStopped()
{
	m_nowPlayingFile.clear();
	m_nowPlayingId = -1;
}


WebRemote::Response WebRemote::handleRequest(const QByteArray& method, const QByteArray& target, bool hasCsrfHeader)
{
	const QUrl url = QUrl::fromEncoded(target);
	const QString path = url.path();
	const QStringList parts = path.split('/', Qt::SkipEmptyParts);

	const bool isGet = (method == "GET");
	const bool isPost = (method == "POST");

	if (isGet)
	{
		Response r;
		if (path == "/" || path == "/index.html")
		{
			r.contentType = "text/html; charset=utf-8";
			r.body = m_indexHtml;
			return r;
		}
		if (path == "/api/state")
		{
			r.body = stateJson();
			return r;
		}
		if (path == "/favicon.ico")
		{
			r.status = 204;
			return r;
		}
		return jsonError(404, "Not found");
	}

	if (!isPost)
		return jsonError(405, "Method not allowed");

	if (parts.value(0) != "api")
		return jsonError(404, "Not found");

	if (!hasCsrfHeader)
		return jsonError(403, "Missing X-RP header");

	const QString action = parts.value(1);

	if (action == "play")
	{
		bool ok = false;
		const int id = parts.value(2).toInt(&ok);
		if (!ok || id < 0 || id >= m_model->getRows() * m_model->getCols())
			return jsonError(400, "Invalid sound id");
		if (!sb_isServerConnected())
			return jsonError(409, "Not connected to a TeamSpeak server");
		const SoundInfo* info = m_model->getSoundInfo(id);
		if (!info || info->filename.isEmpty())
			return jsonError(404, "No sound assigned to this button");
		m_pendingPlayId = id;
		m_pendingConfig = m_model->getConfiguration();
		if (sb_playFile(*info) != 0)
		{
			m_pendingPlayId = -1;
			return jsonError(500, "Could not play sound file");
		}
		return jsonOk();
	}

	if (action == "stop")
	{
		sb_stopPlayback();
		return jsonOk();
	}

	if (action == "pause")
	{
		sb_pauseButtonPressed();
		return jsonOk();
	}

	if (action == "config")
	{
		bool ok = false;
		const int cfg = parts.value(2).toInt(&ok);
		if (!ok || cfg < 0 || cfg >= NUM_CONFIGS)
			return jsonError(400, "Invalid configuration");
		sb_setConfig(cfg);
		return jsonOk();
	}

	if (action == "volume")
	{
		const QUrlQuery q(url);
		bool ok = false;
		if (q.hasQueryItem("remote"))
		{
			const int v = q.queryItemValue("remote").toInt(&ok);
			if (!ok)
				return jsonError(400, "Invalid volume");
			m_model->setVolumeRemote(std::max(0, std::min(100, v)));
		}
		if (q.hasQueryItem("local"))
		{
			const int v = q.queryItemValue("local").toInt(&ok);
			if (!ok)
				return jsonError(400, "Invalid volume");
			m_model->setVolumeLocal(std::max(0, std::min(100, v)));
		}
		return jsonOk();
	}

	return jsonError(404, "Unknown action");
}
