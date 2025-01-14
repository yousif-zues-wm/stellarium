/*
 * Stellarium: Meteor Showers Plug-in
 * Copyright (C) 2013-2015 Marcos Cardinot
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include <QDir>
#include <QJsonDocument>
#include <QSettings>
#include <QTimer>

#include "LabelMgr.hpp"
#include "MSConfigDialog.hpp"
#include "MSSearchDialog.hpp"
#include "MeteorShowersMgr.hpp"
#include "Planet.hpp"
#include "StelActionMgr.hpp"
#include "StelApp.hpp"
#include "StelFileMgr.hpp"
#include "StelGui.hpp"
#include "StelModuleMgr.hpp"
#include "StelProgressController.hpp"
#include "StelTextureMgr.hpp"
#include "StelUtils.hpp"
#include "SporadicMeteorMgr.hpp"

MeteorShowersMgr::MeteorShowersMgr()
	: m_meteorShowers(NULL)
	, m_configDialog(NULL)
	, m_searchDialog(NULL)
	, m_conf(StelApp::getInstance().getSettings())
	, m_onEarth(false)
	, m_enablePlugin(false)
	, m_activeRadiantOnly(false)
	, m_enableAtStartup(true)
	, m_enableLabels(true)
	, m_enableMarker(true)
	, m_showEnableButton(true)
	, m_showSearchButton(true)
	, m_messageTimer(NULL)
	, m_isUpdating(false)
	, m_enableAutoUpdates(true)
	, m_updateFrequencyHours(0)
	, m_statusOfLastUpdate(OUTDATED)
	, m_downloadMgr(NULL)
	, m_progressBar(NULL)
{
	setObjectName("MeteorShowers");
	qsrand(QDateTime::currentMSecsSinceEpoch());
}

MeteorShowersMgr::~MeteorShowersMgr()
{
	delete m_configDialog;
	delete m_searchDialog;
	delete m_downloadMgr;
}

void MeteorShowersMgr::init()
{
	loadTextures();

	m_meteorShowers = new MeteorShowers(this);
	m_configDialog = new MSConfigDialog(this);
	m_searchDialog = new MSSearchDialog(this);

	createActions();
	loadConfig();

	// timer to hide the alert messages
	m_messageTimer = new QTimer(this);
	m_messageTimer->setSingleShot(true);
	m_messageTimer->setInterval(9000);
	m_messageTimer->stop();
	connect(m_messageTimer, SIGNAL(timeout()), this, SLOT(messageTimeout()));

	// MeteorShowers directory
	QString userDir = StelFileMgr::getUserDir() + "/modules/MeteorShowers";
	StelFileMgr::makeSureDirExistsAndIsWritable(userDir);

	// Loads the JSON catalog
	m_catalogPath = userDir + "/showers.json";
	if (!loadCatalog(m_catalogPath))
	{
		displayMessage(q_("The current catalog of Meteor Showers is invalid!"), "#bb0000");
		restoreDefaultCatalog(m_catalogPath);
	}

	// Sets up the download manager
	m_downloadMgr = new QNetworkAccessManager(this);
	connect(m_downloadMgr, SIGNAL(finished(QNetworkReply*)), this, SLOT(updateFinished(QNetworkReply*)));

	// every 5 min, check if it's time to update
	QTimer* updateTimer = new QTimer(this);
	updateTimer->setInterval(300000);
	connect(updateTimer, SIGNAL(timeout()), this, SLOT(checkForUpdates()));
	updateTimer->start();
	checkForUpdates();

	// always check if we are on Earth
	StelCore* core = StelApp::getInstance().getCore();
	m_onEarth = core->getCurrentPlanet().data()->getEnglishName() == "Earth";
	connect(core, SIGNAL(locationChanged(StelLocation)),
		this, SLOT(locationChanged(StelLocation)));

	// enable at startup?
	setEnablePlugin(getEnableAtStartup());
}

void MeteorShowersMgr::deinit()
{
	m_bolideTexture.clear();
	m_radiantTexture.clear();
	m_pointerTexture.clear();
}

double MeteorShowersMgr::getCallOrder(StelModuleActionName actionName) const
{
	if (actionName == StelModule::ActionDraw)
	{
		return GETSTELMODULE(SporadicMeteorMgr)->getCallOrder(actionName) + 10.;
	}
	return 0.;
}

bool MeteorShowersMgr::configureGui(bool show)
{
	if (show)
	{
		m_configDialog->setVisible(show);
	}
	return true;
}

void MeteorShowersMgr::createActions()
{
	QString msGroup = N_("Meteor Showers");
	addAction("actionShow_MeteorShowers",               msGroup, N_("Toggle meteor showers"), this,           "enablePlugin", "Ctrl+Alt+M");
	addAction("actionShow_MeteorShowers_labels",        msGroup, N_("Toggle radiant labels"), this,           "enableLabels", "Shift+M");
	addAction("actionShow_MeteorShowers_config_dialog", msGroup, N_("Show settings dialog"),  m_configDialog, "visible",      "Ctrl+Shift+M");
	addAction("actionShow_MeteorShowers_search_dialog", msGroup, N_("Show search dialog"),    m_searchDialog, "visible",      "Ctrl+Shift+S");
}

void MeteorShowersMgr::loadConfig()
{
	setActiveRadiantOnly(m_conf->value(MS_CONFIG_PREFIX + "/flag_active_radiant_only", true).toBool());
	setShowEnableButton(m_conf->value(MS_CONFIG_PREFIX + "/show_enable_button", true).toBool());
	setShowSearchButton(m_conf->value(MS_CONFIG_PREFIX + "/show_search_button", true).toBool());
	setColorARG(StelUtils::strToVec3f(m_conf->value(MS_CONFIG_PREFIX + "/colorARG", "0,255,240").toString()));
	setColorARC(StelUtils::strToVec3f(m_conf->value(MS_CONFIG_PREFIX + "/colorARC", "255,240,0").toString()));
	setColorIR(StelUtils::strToVec3f(m_conf->value(MS_CONFIG_PREFIX + "/colorIR", "255,255,255").toString()));
	setEnableAtStartup(m_conf->value(MS_CONFIG_PREFIX + "/enable_at_startup", true).toBool());
	setFontSize(m_conf->value(MS_CONFIG_PREFIX + "/font_size", 13).toInt());
	setEnableLabels(m_conf->value(MS_CONFIG_PREFIX + "/flag_radiant_labels", true).toBool());
	setEnableMarker(m_conf->value(MS_CONFIG_PREFIX + "/flag_radiant_marker", true).toBool());
	setUpdateFrequencyHours(m_conf->value(MS_CONFIG_PREFIX + "/update_frequency_hours", 720).toInt());
	setEnableAutoUpdates(m_conf->value(MS_CONFIG_PREFIX + "/automatic_updates_enabled", true).toBool());
	setUrl(m_conf->value(MS_CONFIG_PREFIX + "/url", "http://stellarium.org/json/showers.json").toString());
	setLastUpdate(m_conf->value(MS_CONFIG_PREFIX + "/last_update", "2015-07-01T00:00:00").toDateTime());
	setStatusOfLastUpdate(m_conf->value(MS_CONFIG_PREFIX + "/last_update_status", 0).toInt());
}

void MeteorShowersMgr::loadTextures()
{
	m_bolideTexture = StelApp::getInstance().getTextureManager().createTextureThread(
				StelFileMgr::getInstallationDir() + "/textures/cometComa.png",
				StelTexture::StelTextureParams(true, GL_LINEAR, GL_CLAMP_TO_EDGE));

	m_pointerTexture = StelApp::getInstance().getTextureManager().createTexture(
				StelFileMgr::getInstallationDir() + "/textures/pointeur5.png");

	m_radiantTexture = StelApp::getInstance().getTextureManager().createTexture(
				":/MeteorShowers/radiant.png");
}

bool MeteorShowersMgr::loadCatalog(const QString& jsonPath)
{
	qDebug() << "MeteorShowersMgr: Loading catalog file:"
		 << QDir::toNativeSeparators(jsonPath);

	QFile jsonFile(jsonPath);
	if (!jsonFile.exists())
	{
		restoreDefaultCatalog(jsonPath);
	}

	if (!jsonFile.open(QIODevice::ReadOnly))
	{
		qWarning() << "MeteorShowersMgr: Cannot to open the catalog file!";
		return false;
	}

	QJsonObject json(QJsonDocument::fromJson(jsonFile.readAll()).object());
	jsonFile.close();

	if (json["shortName"].toString() != "meteor showers data"
		|| json["version"].toInt() != MS_CATALOG_VERSION)
	{
		qWarning()  << "MeteorShowersMgr: The current catalog is not compatible!";
		return false;
	}

	QVariantMap map = json["showers"].toObject().toVariantMap();
	m_meteorShowers->loadMeteorShowers(map);

	return true;
}

void MeteorShowersMgr::restoreDefaultSettings()
{
	qDebug() << "MeteorShowersMgr: Restoring default settings";
	m_conf->beginGroup(MS_CONFIG_PREFIX);
	m_conf->remove("");
	m_conf->endGroup();
	loadConfig();
	restoreDefaultCatalog(m_catalogPath);
	m_configDialog->init();
}

bool MeteorShowersMgr::restoreDefaultCatalog(const QString& destination)
{
	qDebug() << "MeteorShowersMgr: Trying to restore the default catalog to"
		 << QDir::toNativeSeparators(destination);

	QFile d(destination);
	if (d.exists() && !d.remove())
	{
		qWarning() << "MeteorShowersMgr: Cannot remove the current catalog file!";
		return false;
	}

	QFile defaultJson(":/MeteorShowers/showers.json");
	if (!defaultJson.copy(destination))
	{
		qWarning() << "MeteorShowersMgr: Cannot copy the default catalog!";
		return false;
	}

	// The resource is read only, and the new file inherits this... make sure the
	// new file is writable by the Stellarium process so that updates can be done.
	QFile dest(destination);
	dest.setPermissions(dest.permissions() | QFile::WriteOwner);

	setLastUpdate(QDateTime::fromString("2015-07-01T00:00:00"));

	qDebug() << "MeteorShowersMgr: The default catalog was copied!";
	displayMessage(q_("Using the default Meteor Showers catalog."), "#bb0000");

	return true;
}

void MeteorShowersMgr::update(double deltaTime)
{
	if (!m_enablePlugin || !m_onEarth)
	{
		return;
	}

	// is paused?
	// freeze meteors at the current position
	if (!StelApp::getInstance().getCore()->getTimeRate()) {
		return;
	}

	m_meteorShowers->update(deltaTime);
}

void MeteorShowersMgr::draw(StelCore* core)
{
	if (m_enablePlugin && m_onEarth)
	{
		m_meteorShowers->draw(core);
	}
}

void MeteorShowersMgr::repaint()
{
	update(1.0);
	draw(StelApp::getInstance().getCore());
}

void MeteorShowersMgr::checkForUpdates()
{
	if (m_enableAutoUpdates && m_lastUpdate.addSecs(m_updateFrequencyHours * 3600.) <= QDateTime::currentDateTime())
	{
		updateCatalog();
	}
}

void MeteorShowersMgr::updateCatalog()
{
	if (m_statusOfLastUpdate == UPDATING)
	{
		qWarning() << "MeteorShowersMgr: The catalog is being updated already!";
		return;
	}

	qDebug() << "MeteorShowersMgr: Starting to update the catalog...";
	setStatusOfLastUpdate(UPDATING);

	setLastUpdate(QDateTime::currentDateTime());

	m_progressBar = StelApp::getInstance().addProgressBar();
	m_progressBar->setValue(0);
	m_progressBar->setRange(0, 100);
	m_progressBar->setFormat("Meteor Showers Catalog");

	QNetworkRequest request;
	request.setUrl(QUrl(m_url));
	request.setRawHeader("User-Agent", QString("Mozilla/5.0 (Stellarium Meteor Showers Plugin %1; http://stellarium.org/)").arg(METEORSHOWERS_PLUGIN_VERSION).toUtf8());
	m_downloadMgr->get(request);
}

void MeteorShowersMgr::updateFinished(QNetworkReply* reply)
{
	if (m_progressBar)
	{
		m_progressBar->setValue(100);
		StelApp::getInstance().removeProgressBar(m_progressBar);
		m_progressBar = NULL;
	}

	if (reply->error() != QNetworkReply::NoError)
	{
		qWarning() << "MeteorShowersMgr: Failed to download!" << reply->url();
		qWarning() << "MeteorShowersMgr: Error " << reply->errorString();
		setStatusOfLastUpdate(FAILED);
		return;
	}

	QString tempPath(m_catalogPath + "_new.json");
	QFile newCatalog(tempPath);
	newCatalog.remove(); // always overwrites
	if (!newCatalog.open(QIODevice::ReadWrite | QIODevice::Text))
	{
		qWarning() << "MeteorShowersMgr: Cannot write the downloaded catalog!";
		setStatusOfLastUpdate(FAILED);
		return;
	}

	newCatalog.write(reply->readAll());
	newCatalog.close();

	if (!loadCatalog(tempPath))
	{
		setStatusOfLastUpdate(FAILED);
		return;
	}

	QFile(m_catalogPath).remove();
	newCatalog.rename(tempPath.remove("_new.json"));

	qDebug() << "MeteorShowersMgr: The catalog was updated!";
	setStatusOfLastUpdate(UPDATED);
}

void MeteorShowersMgr::setEnablePlugin(const bool& b)
{
	// we should never change the 'm_enablePlugin' member directly!
	// as it's a button on the toolbar, it must be sync with its StelAction
	StelActionMgr* actionMgr = StelApp::getInstance().getStelActionManager();
	StelAction* action = actionMgr->findAction("actionShow_MeteorShowers");
	action->setChecked(b);
}

void MeteorShowersMgr::setActiveRadiantOnly(const bool& b)
{
	m_activeRadiantOnly = b;
	m_conf->setValue(MS_CONFIG_PREFIX + "/flag_active_radiant_only", b);
}

void MeteorShowersMgr::setShowEnableButton(const bool& show)
{
	try
	{
		StelGui* gui = dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
		if (!gui)
		{
			return;
		}

		if (show)
		{
			StelButton* enablePlugin = new StelButton(NULL,
								  QPixmap(":/MeteorShowers/btMS-on.png"),
								  QPixmap(":/MeteorShowers/btMS-off.png"),
								  QPixmap(":/graphicGui/glow32x32.png"),
								  "actionShow_MeteorShowers");
			gui->getButtonBar()->addButton(enablePlugin, "065-pluginsGroup");
		}
		else
		{
			gui->getButtonBar()->hideButton("actionShow_MeteorShowers");
		}
	}
	catch (std::runtime_error& e)
	{
		qWarning() << "MeteorShowersMgr : unable to create toolbar buttons for MeteorShowers plugin!"
			   << e.what();
	}
	m_showEnableButton = show;
	m_conf->setValue(MS_CONFIG_PREFIX + "/show_enable_button", show);
}

void MeteorShowersMgr::setShowSearchButton(const bool& show)
{
	try
	{
		StelGui* gui = dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
		if (!gui)
		{
			return;
		}

		if (show)
		{
			StelButton* searchMS = new StelButton(NULL,
							      QPixmap(":/MeteorShowers/btMS-search-on.png"),
							      QPixmap(":/MeteorShowers/btMS-search-off.png"),
							      QPixmap(":/graphicGui/glow32x32.png"),
							      "actionShow_MeteorShowers_search_dialog");
			gui->getButtonBar()->addButton(searchMS, "065-pluginsGroup");
		}
		else
		{
			gui->getButtonBar()->hideButton("actionShow_MeteorShowers_search_dialog");
		}
	}
	catch (std::runtime_error& e)
	{
		qWarning() << "MeteorShowersMgr : unable to create toolbar buttons for MeteorShowers plugin!"
			   << e.what();
	}
	m_showSearchButton = show;
	m_conf->setValue(MS_CONFIG_PREFIX + "/show_search_button", show);
}

void MeteorShowersMgr::setColorARG(const Vec3f& rgb)
{
	m_colorARG = rgb;
	QString rgbStr = QString("%1,%2,%3").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]);
	m_conf->setValue(MS_CONFIG_PREFIX + "/colorARG", rgbStr);
}

void MeteorShowersMgr::setColorARC(const Vec3f& rgb)
{
	m_colorARC = rgb;
	QString rgbStr = QString("%1,%2,%3").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]);
	m_conf->setValue(MS_CONFIG_PREFIX + "/colorARC", rgbStr);
}

void MeteorShowersMgr::setColorIR(const Vec3f& rgb)
{
	m_colorIR = rgb;
	QString rgbStr = QString("%1,%2,%3").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]);
	m_conf->setValue(MS_CONFIG_PREFIX + "/colorIR", rgbStr);
}

void MeteorShowersMgr::setEnableAtStartup(const bool& b)
{
	m_enableAtStartup = b;
	m_conf->setValue(MS_CONFIG_PREFIX + "/enable_at_startup", b);
}

void MeteorShowersMgr::setFontSize(int pixelSize)
{
	pixelSize = pixelSize < 1 ? 13 : pixelSize;
	m_font.setPixelSize(pixelSize);
	m_conf->setValue(MS_CONFIG_PREFIX + "/font_size", pixelSize);
}

void MeteorShowersMgr::setEnableLabels(const bool& b)
{
	m_enableLabels = b;
	m_conf->setValue(MS_CONFIG_PREFIX + "/flag_radiant_labels", b);
}

void MeteorShowersMgr::setEnableMarker(const bool& b)
{
	m_enableMarker = b;
	m_conf->setValue(MS_CONFIG_PREFIX + "/flag_radiant_marker", b);
}

void MeteorShowersMgr::setUpdateFrequencyHours(const int& hours)
{
	m_updateFrequencyHours = hours;
	m_conf->setValue(MS_CONFIG_PREFIX + "/update_frequency_hours", hours);
}

void MeteorShowersMgr::setEnableAutoUpdates(const bool& b)
{
	m_enableAutoUpdates = b;
	m_conf->setValue(MS_CONFIG_PREFIX + "/automatic_updates_enabled", b);
}

void MeteorShowersMgr::setUrl(const QString& url)
{
	m_url = url;
	m_conf->setValue(MS_CONFIG_PREFIX + "/url", url);
}

void MeteorShowersMgr::setLastUpdate(const QDateTime &datetime)
{
	m_lastUpdate = datetime;
	m_conf->setValue(MS_CONFIG_PREFIX + "/last_update", m_lastUpdate.toString(Qt::ISODate));
}

void MeteorShowersMgr::setStatusOfLastUpdate(const int &downloadStatus)
{
	m_statusOfLastUpdate = (DownloadStatus) downloadStatus;
	if (m_statusOfLastUpdate != UPDATING)
	{
		m_conf->setValue(MS_CONFIG_PREFIX + "/last_update_status", downloadStatus);
	}
	emit(downloadStatusChanged(m_statusOfLastUpdate));
}

QDateTime MeteorShowersMgr::getNextUpdate()
{
	return m_lastUpdate.addSecs(m_updateFrequencyHours * 3600);
}

void MeteorShowersMgr::displayMessage(const QString& message, const QString hexColor)
{
	m_messageIDs << GETSTELMODULE(LabelMgr)->labelScreen(message, 30, 30 + (20 * m_messageIDs.count()), true, 16, hexColor);
	m_messageTimer->start();
}

void MeteorShowersMgr::messageTimeout()
{
	foreach(int i, m_messageIDs)
	{
		GETSTELMODULE(LabelMgr)->deleteLabel(i);
	}
}

void MeteorShowersMgr::locationChanged(StelLocation location)
{
	m_onEarth = location.planetName == "Earth";
}

/////////////////////////////////////////////////////////////////////////////////////////

StelModule* MeteorShowersStelPluginInterface::getStelModule() const
{
	return new MeteorShowersMgr();
}

StelPluginInfo MeteorShowersStelPluginInterface::getPluginInfo() const
{
	// Allow to load the resources when used as a static plugin
	Q_INIT_RESOURCE(MeteorShower);

	StelPluginInfo info;
	info.id = "MeteorShowers";
	info.displayedName = N_("Meteor Showers");
	info.authors = "Marcos Cardinot";
	info.contact = "mcardinot@gmail.com";
	info.description = N_(
	"<p>"
		"This plugin enables you to simulate periodic meteor showers and "
		"to display a marker for each active and inactive radiant."
	"</p>"
	"<p>"
		"By a single click on the radiant's marker, you can see all the "
		"details about its position and activity. Most data used on this "
		"plugin comes from the official <a href=\"http://imo.net\">International "
		"Meteor Organization</a> catalog."
	"</p>"
	"<p>"
		"It has three types of markers:"
		"<ul>"
			"<li>"
				"<b>Confirmed:</b> "
				"the radiant is active and its data was confirmed."
				" Thus, this is a historical (really occurred in the past) or predicted"
				" meteor shower."
			"</li>"
			"<li>"
				"<b>Generic:</b> "
				"the radiant is active, but its data was not confirmed."
				" It means that this can occur in real life, but that we do not have proper"
				" data about its activity for the current year."
			"</li>"
			"<li>"
				"<b>Inactive:</b> "
				"the radiant is inactive for the current sky date."
			"</li>"
		"</ul>"
	"</p>");
	info.version = METEORSHOWERS_PLUGIN_VERSION;
	return info;
}
