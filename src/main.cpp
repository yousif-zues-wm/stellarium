/*
 * Stellarium
 * Copyright (C) 2002 Fabien Chereau
 * Copyright (C) 2012 Timothy Reaves
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

#include "StelMainView.hpp"
#include "StelTranslator.hpp"
#include "StelLogger.hpp"
#include "StelFileMgr.hpp"
#include "CLIProcessor.hpp"
#include "StelIniParser.hpp"
#include "StelUtils.hpp"
#ifndef DISABLE_SCRIPTING
#include "StelScriptOutput.hpp"
#endif

#include <QDebug>

#ifndef USE_QUICKVIEW
	#include <QApplication>
	#include <QMessageBox>
	#include <QStyleFactory>
#else
	#include <QGuiApplication>
#endif
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QSettings>
#include <QSplashScreen>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTranslator>
#include <QNetworkDiskCache>

#include <clocale>

#ifdef Q_OS_WIN
	#include <windows.h>
	#ifdef _MSC_BUILD
		#include <MMSystem.h>
		#pragma comment(lib,"Winmm.lib")
		#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup") // Hide console
	#endif
#endif //Q_OS_WIN

//! @class CustomQTranslator
//! Provides custom i18n support.
class CustomQTranslator : public QTranslator
{
	using QTranslator::translate;
public:
	virtual bool isEmpty() const { return false; }

	//! Overrides QTranslator::translate().
	//! Calls StelTranslator::qtranslate().
	//! @param context Qt context string - IGNORED.
	//! @param sourceText the source message.
	//! @param comment optional parameter
	virtual QString translate(const char *context, const char *sourceText, const char *disambiguation = 0, int n = -1) const
	{
		Q_UNUSED(context);
		Q_UNUSED(n);
		return StelTranslator::globalTranslator->qtranslate(sourceText, disambiguation);
	}
};


//! Copies the default configuration file.
//! This function copies the default_config.ini file to config.ini (or other
//! name specified on the command line located in the user data directory.
void copyDefaultConfigFile(const QString& newPath)
{
	QString defaultConfigFilePath = StelFileMgr::findFile("data/default_config.ini");
	if (defaultConfigFilePath.isEmpty())
		qFatal("ERROR copyDefaultConfigFile failed to locate data/default_config.ini. Please check your installation.");
	QFile::copy(defaultConfigFilePath, newPath);
	if (!StelFileMgr::exists(newPath))
	{
		qFatal("ERROR copyDefaultConfigFile failed to copy file %s  to %s. You could try to copy it by hand.",
			qPrintable(defaultConfigFilePath), qPrintable(newPath));
	}
	QFile::setPermissions(newPath, QFile::permissions(newPath) | QFileDevice::WriteOwner);
}

//! Removes all items from the cache.
void clearCache()
{
	QNetworkDiskCache* cacheMgr = new QNetworkDiskCache();
	cacheMgr->setCacheDirectory(StelFileMgr::getCacheDir());
	cacheMgr->clear(); // Removes all items from the cache.
}

void registerPluginsDir(QDir& appDir)
{
	QStringList pathes;
	// Windows
	pathes << appDir.absolutePath();
	pathes << appDir.absoluteFilePath("platforms");
	// OS X
	appDir.cdUp();	
	pathes << appDir.absoluteFilePath("plugins");
	// All systems
	pathes << QCoreApplication::libraryPaths();
	QCoreApplication::setLibraryPaths(pathes);
}

// Main stellarium procedure
int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
	// Fix for the speeding system clock bug on systems that use ACPI
	// See http://support.microsoft.com/kb/821893
	UINT timerGrain = 1;
	if (timeBeginPeriod(timerGrain) == TIMERR_NOCANDO)
	{
		// If this is too fine a grain, try the lowest value used by a timer
		timerGrain = 5;
		if (timeBeginPeriod(timerGrain) == TIMERR_NOCANDO)
			timerGrain = 0;
	}
#endif

	QCoreApplication::setApplicationName("stellarium");
	QCoreApplication::setApplicationVersion(StelUtils::getApplicationVersion());
	QCoreApplication::setOrganizationDomain("stellarium.org");
	QCoreApplication::setOrganizationName("stellarium");

	// LP:1335611: Avoid troubles with search of the paths of the plugins (deployments troubles) --AW
	QFileInfo appInfo(QString::fromUtf8(argv[0]));
	QDir appDir(appInfo.absolutePath());
	registerPluginsDir(appDir);

	QGuiApplication::setDesktopSettingsAware(false);

#ifndef USE_QUICKVIEW
	QApplication::setStyle(QStyleFactory::create("Fusion"));
	// The QApplication MUST be created before the StelFileMgr is initialized.
	QApplication app(argc, argv);
#else
	QGuiApplication::setDesktopSettingsAware(false);
	QGuiApplication app(argc, argv);
#endif
	QPixmap pixmap(":/splash.png");
	QSplashScreen splash(pixmap);
	splash.show();
	app.processEvents();

	// QApplication sets current locale, but
	// we need scanf()/printf() and friends to always work in the C locale,
	// otherwise configuration/INI file parsing will be erroneous.
	setlocale(LC_NUMERIC, "C");

	// Init the file manager
	StelFileMgr::init();

	// Log command line arguments
	QString argStr;
	QStringList argList;
	for (int i=0; i<argc; ++i)
	{
		argList << argv[i];
		argStr += QString("%1 ").arg(argv[i]);
	}
	// Parse for first set of CLI arguments - stuff we want to process before other
	// output, such as --help and --version
	CLIProcessor::parseCLIArgsPreConfig(argList);

	// Start logging.
	StelLogger::init(StelFileMgr::getUserDir()+"/log.txt");
	StelLogger::writeLog(argStr);

	// OK we start the full program.
	// Print the console splash and get on with loading the program
	QString versionLine = QString("This is %1 - http://www.stellarium.org").arg(StelUtils::getApplicationName());
	QString copyrightLine = QString("Copyright (C) 2000-2015 Fabien Chereau et al.");
	int maxLength = qMax(versionLine.size(), copyrightLine.size());
	qDebug() << qPrintable(QString(" %1").arg(QString().fill('-', maxLength+2)));
	qDebug() << qPrintable(QString("[ %1 ]").arg(versionLine.leftJustified(maxLength, ' ')));
	qDebug() << qPrintable(QString("[ %1 ]").arg(copyrightLine.leftJustified(maxLength, ' ')));
	qDebug() << qPrintable(QString(" %1").arg(QString().fill('-', maxLength+2)));
	qDebug() << "Writing log file to:" << QDir::toNativeSeparators(StelLogger::getLogFileName());
	qDebug() << "File search paths:";
	int n=0;
	foreach (const QString& i, StelFileMgr::getSearchPaths())
	{
		qDebug() << " " << n << ". " << QDir::toNativeSeparators(i);
		++n;
	}

	// Now manage the loading of the proper config file
	QString configName;
	try
	{
		configName = CLIProcessor::argsGetOptionWithArg(argList, "-c", "--config-file", "config.ini").toString();
	}
	catch (std::runtime_error& e)
	{
		qWarning() << "WARNING: while looking for --config-file option: " << e.what() << ". Using \"config.ini\"";
		configName = "config.ini";
	}

	QString configFileFullPath = StelFileMgr::findFile(configName, StelFileMgr::Flags(StelFileMgr::Writable|StelFileMgr::File));
	if (configFileFullPath.isEmpty())
	{
		configFileFullPath = StelFileMgr::findFile(configName, StelFileMgr::New);
		if (configFileFullPath.isEmpty())
			qFatal("Could not create configuration file %s.", qPrintable(configName));
	}

	QSettings* confSettings = NULL;
	if (StelFileMgr::exists(configFileFullPath))
	{
		// Implement "restore default settings" feature.
		bool restoreDefaultConfigFile = false;
		if (CLIProcessor::argsGetOption(argList, "", "--restore-defaults"))
		{
			restoreDefaultConfigFile=true;
		}
		else
		{
			confSettings = new QSettings(configFileFullPath, StelIniFormat, NULL);
			restoreDefaultConfigFile = confSettings->value("main/restore_defaults", false).toBool();
		}
		if (!restoreDefaultConfigFile)
		{
			QString version = confSettings->value("main/version", "0.0.0").toString();
			if (version!=QString(PACKAGE_VERSION))
			{
				QTextStream istr(&version);
				char tmp;
				int v1=0;
				int v2=0;
				istr >> v1 >> tmp >> v2;
				// Config versions less than 0.6.0 are not supported, otherwise we will try to use it
				if (v1==0 && v2<6)
				{
					// The config file is too old to try an importation
					qDebug() << "The current config file is from a version too old for parameters to be imported ("
							 << (version.isEmpty() ? "<0.6.0" : version) << ").\n"
							 << "It will be replaced by the default config file.";
					restoreDefaultConfigFile = true;
				}
				else
				{
					qDebug() << "Attempting to use an existing older config file.";
					confSettings->setValue("main/version", QString(PACKAGE_VERSION)); // Upgrade version of config.ini
					clearCache();
					qDebug() << "Clear cache and update config.ini...";
				}
			}
		}
		if (restoreDefaultConfigFile)
		{
			if (confSettings)
				delete confSettings;
			QString backupFile(configFileFullPath.left(configFileFullPath.length()-3) + QString("old"));
			if (QFileInfo(backupFile).exists())
				QFile(backupFile).remove();
			QFile(configFileFullPath).rename(backupFile);
			copyDefaultConfigFile(configFileFullPath);
			confSettings = new QSettings(configFileFullPath, StelIniFormat);
			qWarning() << "Resetting defaults config file. Previous config file was backed up in " << QDir::toNativeSeparators(backupFile);
			clearCache();
		}
	}
	else
	{
		qDebug() << "Config file " << QDir::toNativeSeparators(configFileFullPath) << " does not exist. Copying the default file.";
		copyDefaultConfigFile(configFileFullPath);
		confSettings = new QSettings(configFileFullPath, StelIniFormat);
	}

	Q_ASSERT(confSettings);
	qDebug() << "Config file is: " << QDir::toNativeSeparators(configFileFullPath);

	#ifndef DISABLE_SCRIPTING
	QString outputFile = StelFileMgr::getUserDir()+"/output.txt";
	if (confSettings->value("main/use_separate_output_file", false).toBool())
		outputFile = StelFileMgr::getUserDir()+"/output-"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")+".txt";
	StelScriptOutput::init(outputFile);
	#endif


	// Override config file values from CLI.
	CLIProcessor::parseCLIArgsPostConfig(argList, confSettings);

	// Support hi-dpi pixmaps
	app.setAttribute(Qt::AA_UseHighDpiPixmaps);

	// Add the DejaVu font that we use everywhere in the program
	const QString& fName = StelFileMgr::findFile("data/DejaVuSans.ttf");
	if (!fName.isEmpty())
		QFontDatabase::addApplicationFont(fName);
	
	QString fileFont = confSettings->value("gui/base_font_file", "").toString();
	if (!fileFont.isEmpty())
	{
		const QString& afName = StelFileMgr::findFile(QString("data/%1").arg(fileFont));
		if (!afName.isEmpty() && !afName.contains("file not found"))
			QFontDatabase::addApplicationFont(afName);
		else
			qWarning() << "ERROR while loading custom font " << QDir::toNativeSeparators(fileFont);
	}

	// Set the default application font and font size.
	// Note that style sheet will possibly override this setting.
#ifdef Q_OS_WIN
	// Let's try avoid ugly font rendering on Windows.
	// Details: https://sourceforge.net/p/stellarium/discussion/278769/thread/810a1e5c/
	QString baseFont = confSettings->value("gui/base_font_name", "Verdana").toString();
	QFont tmpFont(baseFont);
	tmpFont.setStyleHint(QFont::AnyStyle, QFont::OpenGLCompatible);
#else
	QString baseFont = confSettings->value("gui/base_font_name", "DejaVu Sans").toString();
	QFont tmpFont(baseFont);
#endif
	tmpFont.setPixelSize(confSettings->value("gui/base_font_size", 13).toInt());
	QGuiApplication::setFont(tmpFont);

	// Initialize translator feature
	StelTranslator::init(StelFileMgr::getInstallationDir() + "/data/iso639-1.utf8");
	
	// Use our custom translator for Qt translations as well
	CustomQTranslator trans;
	app.installTranslator(&trans);

	StelMainView mainWin;
	mainWin.init(confSettings); // May exit(0) when OpenGL subsystem insufficient
	splash.finish(&mainWin);
	app.exec();
	mainWin.deinit();

	delete confSettings;
	StelLogger::deinit();

	#ifdef Q_OS_WIN
	if(timerGrain)
		timeEndPeriod(timerGrain);
	#endif //Q_OS_WIN

	return 0;
}

