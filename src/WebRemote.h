// src/WebRemote.h
//----------------------------------
// Small embedded HTTP server so the soundboard can be controlled from a
// phone/tablet/browser on the local network.
//----------------------------------

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>

class QTcpServer;
class QTcpSocket;
class ConfigModel;

class WebRemote : public QObject
{
	Q_OBJECT

  public:
	explicit WebRemote(ConfigModel* model, QObject* parent = nullptr);
	~WebRemote() override;

	/** Start listening on all network interfaces. Returns false on failure, see lastError(). */
	bool start(quint16 port);
	void stop();

	bool isRunning() const;
	quint16 port() const;
	QString lastError() const
	{
		return m_lastError;
	}

	/** URLs under which this machine is reachable from the LAN, e.g. "http://192.168.1.20:8080/" */
	static QStringList lanUrls(quint16 port);

  private slots:
	void onNewConnection();
	void onPlaybackStarted(bool preview, QString filename);
	void onPlaybackStopped();

  private:
	struct Response
	{
		int status = 200;
		QByteArray contentType = "application/json; charset=utf-8";
		QByteArray body;
	};

	void onReadyRead(QTcpSocket* sock);
	void respond(QTcpSocket* sock, const Response& r);
	Response handleRequest(const QByteArray& method, const QByteArray& target, bool hasCsrfHeader);
	Response jsonError(int status, const QString& msg) const;
	Response jsonOk() const;
	QByteArray stateJson() const;
	QString soundDisplayName(int id) const;

	ConfigModel* m_model;
	QTcpServer* m_server;
	QString m_lastError;
	QByteArray m_indexHtml;
	QString m_nowPlayingFile;
	int m_nowPlayingId = -1;
	int m_nowPlayingConfig = -1;
	int m_pendingPlayId = -1;
	int m_pendingConfig = -1;
};
