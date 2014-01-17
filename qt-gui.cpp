/* qt-gui.cpp */
/* Qt UI implementation */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ctype.h>

#include <libxslt/documents.h>

#include "dive.h"
#include "divelist.h"
#include "display.h"
#include "uemis.h"
#include "device.h"
#include "webservice.h"
#include "libdivecomputer.h"
#include "qt-ui/mainwindow.h"
#include "helpers.h"
#include "qthelper.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QStringList>
#include <QTextCodec>
#include <QTranslator>
#include <QSettings>
#include <QDesktopWidget>
#include <QStyle>
#include <QDebug>
#include <QMap>
#include <QMultiMap>
#include <QNetworkProxy>
#include <QDateTime>
#include <QRegExp>
#include <QResource>
#include <QLibraryInfo>

#include <gettextfromc.h>

// this will create a warning when executing lupdate
#define translate(_context, arg) gettextFromC::instance()->tr(arg)

const char *default_dive_computer_vendor;
const char *default_dive_computer_product;
const char *default_dive_computer_device;
DiveComputerList dcList;

static QApplication *application = NULL;
static MainWindow *window = NULL;

int        error_count;
const char *existing_filename;

const char *getSetting(QSettings &s, QString name)
{
	QVariant v;
	v = s.value(name);
	if (v.isValid()) {
		return strdup(v.toString().toUtf8().data());
	}
	return NULL;
}

#ifdef Q_OS_WIN
static QByteArray encodeUtf8(const QString &fname)
{
	return fname.toUtf8();
}

static QString decodeUtf8(const QByteArray &fname)
{
	return QString::fromUtf8(fname);
}
#endif

void init_ui(int *argcp, char ***argvp)
{
	QVariant v;

	application = new QApplication(*argcp, *argvp);

	// tell Qt to use system proxies
	// note: on Linux, "system" == "environment variables"
	QNetworkProxyFactory::setUseSystemConfiguration(true);

#if QT_VERSION < 0x050000
	// ask QString in Qt 4 to interpret all char* as UTF-8,
	// like Qt 5 does.
	// 106 is "UTF-8", this is faster than lookup by name
	// [http://www.iana.org/assignments/character-sets/character-sets.xml]
	QTextCodec::setCodecForCStrings(QTextCodec::codecForMib(106));
#  ifdef Q_OS_WIN
	QFile::setDecodingFunction(decodeUtf8);
	QFile::setEncodingFunction(encodeUtf8);
#  endif
#endif
	QCoreApplication::setOrganizationName("Subsurface");
	QCoreApplication::setOrganizationDomain("subsurface.hohndel.org");
	QCoreApplication::setApplicationName("Subsurface");
	// find plugins installed in the application directory (without this SVGs don't work on Windows)
	QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath());

	QSettings s;
	s.beginGroup("Language");
	QLocale loc;

	if (!s.value("UseSystemLanguage", true).toBool()){
		loc = QLocale(s.value("UiLanguage", QLocale().uiLanguages().first()).toString());
	}

	QString uiLang = loc.uiLanguages().first();
	s.endGroup();

	// there's a stupid Qt bug on MacOS where uiLanguages doesn't give us the country info
	if (!uiLang.contains('-') && uiLang != loc.bcp47Name()) {
		QLocale loc2(loc.bcp47Name());
		loc = loc2;
		uiLang = loc2.uiLanguages().first();
	}

	// we don't have translations for English - if we don't check for this
	// Qt will proceed to load the second language in preference order - not what we want
	// on Linux this tends to be en-US, but on the Mac it's just en
	if (!uiLang.startsWith("en")) {
		qtTranslator = new QTranslator;
		if (qtTranslator->load(loc,"qt", "_", QLibraryInfo::location(QLibraryInfo::TranslationsPath))) {
			application->installTranslator(qtTranslator);
		} else {
			qDebug() << "can't find Qt localization for locale" << uiLang <<
				    "searching in" << QLibraryInfo::location(QLibraryInfo::TranslationsPath);
		}
		ssrfTranslator = new QTranslator;
		if (ssrfTranslator->load(loc,"subsurface", "_") ||
				ssrfTranslator->load(loc,"subsurface", "_", getSubsurfaceDataPath("translations")) ||
				ssrfTranslator->load(loc,"subsurface", "_", getSubsurfaceDataPath("../translations"))) {
			application->installTranslator(ssrfTranslator);
		} else {
			qDebug() << "can't find Subsurface localization for locale" << uiLang;
		}
	}

	s.beginGroup("DiveComputer");
	default_dive_computer_vendor = getSetting(s, "dive_computer_vendor");
	default_dive_computer_product = getSetting(s,"dive_computer_product");
	default_dive_computer_device = getSetting(s, "dive_computer_device");
	s.endGroup();

	window = new MainWindow();
	window->show();
	if (existing_filename && existing_filename[0] != '\0')
		window->setTitle(MWTF_FILENAME);
	else
		window->setTitle(MWTF_DEFAULT);

	return;
}

void run_ui(void)
{
	application->exec();
}

void exit_ui(void)
{
	delete window;
	delete application;
	if (existing_filename)
		free((void *)existing_filename);
	if (default_dive_computer_device)
		free((void *)default_dive_computer_device);
}

void set_filename(const char *filename, bool force)
{
	if (!force && existing_filename)
		return;
	free((void *)existing_filename);
	if (filename)
		existing_filename = strdup(filename);
	else
		existing_filename = NULL;
}

const QString get_dc_nickname(const char *model, uint32_t deviceid)
{
	const DiveComputerNode *existNode = dcList.getExact(model, deviceid);
	if (!existNode)
		return QString();
	else if (!existNode->nickName.isEmpty())
		return existNode->nickName;
	else
		return model;
}

void set_dc_nickname(struct dive *dive)
{
	if (!dive)
		return;

	struct divecomputer *dc = &dive->dc;

	while (dc) {
		if (dc->model && *dc->model && dc->deviceid &&
		    !dcList.getExact(dc->model, dc->deviceid)) {
			// we don't have this one, yet
			const DiveComputerNode *existNode = dcList.get(dc->model);
			if (existNode) {
				// we already have this model but a different deviceid
				QString simpleNick(dc->model);
				if (dc->deviceid == 0)
					simpleNick.append(" (unknown deviceid)");
				else
					simpleNick.append(" (").append(QString::number(dc->deviceid, 16)).append(")");
				dcList.addDC(dc->model, dc->deviceid, simpleNick);
			} else {
				dcList.addDC(dc->model, dc->deviceid);
			}
		}
		dc = dc->next;
	}
}

QString get_depth_string(int mm, bool showunit, bool showdecimal)
{
	if (prefs.units.length == units::METERS) {
		double meters = mm / 1000.0;
		return QString("%1%2").arg(meters, 0, 'f', (showdecimal && meters < 20.0) ? 1 : 0 ).arg(showunit ? translate("gettextFromC","m") : "");
	} else {
		double feet = mm_to_feet(mm);
		return QString("%1%2").arg(feet, 0, 'f', showdecimal ? 1 : 0). arg(showunit ? translate("gettextFromC","ft") : "");
	}
}

QString get_depth_string(depth_t depth, bool showunit, bool showdecimal)
{
	return get_depth_string(depth.mm, showunit, showdecimal);
}

QString get_depth_unit()
{
	if (prefs.units.length == units::METERS)
		return QString("%1").arg(translate("gettextFromC","m"));
	else
		return QString("%1").arg(translate("gettextFromC","ft"));
}

QString get_weight_string(weight_t weight, bool showunit)
{
	QString str = weight_string (weight.grams);
	if (get_units()->weight == units::KG) {
		str = QString ("%1%2").arg(str).arg(showunit ? translate("gettextFromC","kg") : "");
	} else {
		str = QString ("%1%2").arg(str).arg(showunit ? translate("gettextFromC","lbs") : "");
	}
	return (str);
}

QString get_weight_unit()
{
	if (prefs.units.weight == units::KG)
		return QString("%1").arg(translate("gettextFromC","kg"));
	else
		return QString("%1").arg(translate("gettextFromC","lbs"));
}

/* these methods retrieve used gas per cylinder */
static unsigned start_pressure(cylinder_t *cyl)
{
	return cyl->start.mbar ? : cyl->sample_start.mbar;
}

static unsigned end_pressure(cylinder_t *cyl)
{
	return cyl->end.mbar ? : cyl->sample_end.mbar;
}

QString get_cylinder_used_gas_string(cylinder_t *cyl, bool showunit)
{
	int decimals;
	const char *unit;
	double gas_usage;
	/* Get the cylinder gas use in mbar */
	gas_usage = start_pressure(cyl) - end_pressure(cyl);
	/* Can we turn it into a volume? */
	if (cyl->type.size.mliter) {
		gas_usage = bar_to_atm(gas_usage / 1000);
		gas_usage *= cyl->type.size.mliter;
		gas_usage = get_volume_units(gas_usage, &decimals, &unit);
	} else {
		gas_usage = get_pressure_units(gas_usage, &unit);
		decimals = 0;
	}
	// translate("gettextFromC","%.*f %s"
	return QString("%1 %2").arg(gas_usage, 0, 'f', decimals).arg(showunit ? unit : "");
}

QString get_temperature_string(temperature_t temp, bool showunit)
{
	if (temp.mkelvin == 0) {
		return "";  //temperature not defined
	} else if (prefs.units.temperature == units::CELSIUS) {
		double celsius = mkelvin_to_C(temp.mkelvin);
		return QString("%1%2%3").arg(celsius, 0, 'f', 1).arg(showunit ? (UTF8_DEGREE): "")
								.arg(showunit ? translate("gettextFromC","C") : "");
	} else {
		double fahrenheit = mkelvin_to_F(temp.mkelvin);
		return QString("%1%2%3").arg(fahrenheit, 0, 'f', 1).arg(showunit ? (UTF8_DEGREE): "")
								.arg(showunit ? translate("gettextFromC","F") : "");
	}
}

QString get_temp_unit()
{
	if (prefs.units.temperature == units::CELSIUS)
		return QString(UTF8_DEGREE "C");
	else
		return QString(UTF8_DEGREE "F");
}

QString get_volume_string(volume_t volume, bool showunit, unsigned int mbar)
{
	if (prefs.units.volume == units::LITER) {
		double liter = volume.mliter / 1000.0;
		return QString("%1%2").arg(liter, 0, 'f', liter >= 40.0 ? 0 : 1 ).arg(showunit ? translate("gettextFromC","l") : "");
	} else {
		double cuft = ml_to_cuft(volume.mliter);
		if (mbar)
			cuft *= bar_to_atm(mbar / 1000.0);
		return QString("%1%2").arg(cuft, 0, 'f', cuft >= 20.0 ? 0 : (cuft >= 2.0 ? 1 : 2)).arg(showunit ? translate("gettextFromC","cuft") : "");
	}
}

QString get_volume_unit()
{
	if (prefs.units.volume == units::LITER)
		return "l";
	else
		return "cuft";
}

QString get_pressure_string(pressure_t pressure, bool showunit)
{
	if (prefs.units.pressure == units::BAR) {
		double bar = pressure.mbar / 1000.0;
		return QString("%1%2").arg(bar, 0, 'f', 1).arg(showunit ? translate("gettextFromC","bar") : "");
	} else {
		double psi = mbar_to_PSI(pressure.mbar);
		return QString("%1%2").arg(psi, 0, 'f', 0).arg(showunit ? translate("gettextFromC","psi") : "");
	}
}

double get_screen_dpi()
{
	QDesktopWidget *mydesk = application->desktop();
	return mydesk->physicalDpiX();
}

int is_default_dive_computer(const char *vendor, const char *product)
{
	return default_dive_computer_vendor && !strcmp(vendor, default_dive_computer_vendor) &&
		default_dive_computer_product && !strcmp(product, default_dive_computer_product);
}

int is_default_dive_computer_device(const char *name)
{
	return default_dive_computer_device && !strcmp(name, default_dive_computer_device);
}

void set_default_dive_computer(const char *vendor, const char *product)
{
	QSettings s;

	if (!vendor || !*vendor)
		return;
	if (!product || !*product)
		return;
	if (is_default_dive_computer(vendor, product))
		return;
	if (default_dive_computer_vendor)
		free((void *)default_dive_computer_vendor);
	if (default_dive_computer_product)
		free((void *)default_dive_computer_product);
	default_dive_computer_vendor = strdup(vendor);
	default_dive_computer_product = strdup(product);
	s.beginGroup("DiveComputer");
	s.setValue("dive_computer_vendor", vendor);
	s.setValue("dive_computer_product", product);
	s.endGroup();
}

void set_default_dive_computer_device(const char *name)
{
	QSettings s;

	if (!name || !*name)
		return;
	if (is_default_dive_computer_device(name))
		return;
	if (default_dive_computer_device)
		free((void *)default_dive_computer_device);
	default_dive_computer_device = strdup(name);
	s.beginGroup("DiveComputer");
	s.setValue("dive_computer_device", name);
	s.endGroup();
}

QString getSubsurfaceDataPath(QString folderToFind)
{
	QString execdir;
	QDir folder;

	// first check if we are running in the build dir, so the path that we
	// are looking for is just a  subdirectory of the execution path;
	// this also works on Windows as there we install the dirs
	// under the application path
	execdir = QCoreApplication::applicationDirPath();
	folder = QDir(execdir.append(QDir::separator()).append(folderToFind));
	if (folder.exists())
		return folder.absolutePath();

	// next check for the Linux typical $(prefix)/share/subsurface
	execdir = QCoreApplication::applicationDirPath();
	if (execdir.contains("bin")) {
		folder = QDir(execdir.replace("bin", "share/subsurface/").append(folderToFind));
		if (folder.exists())
			return folder.absolutePath();
	}
	// then look for the usual locations on a Mac
	execdir = QCoreApplication::applicationDirPath();
	folder = QDir(execdir.append("/../Resources/share/").append(folderToFind));
	if (folder.exists())
		return folder.absolutePath();
	execdir = QCoreApplication::applicationDirPath();
	folder = QDir(execdir.append("/../Resources/").append(folderToFind));
	if (folder.exists())
		return folder.absolutePath();
	return QString("");
}

void create_device_node(const char *model, uint32_t deviceid, const char *serial, const char *firmware, const char *nickname)
{
	dcList.addDC(model, deviceid, nickname, serial, firmware);
}

bool compareDC(const DiveComputerNode &a, const DiveComputerNode &b)
{
	return a.deviceId < b.deviceId;
}

void call_for_each_dc(void *f, void (*callback)(void *, const char *, uint32_t,
						const char *, const char *, const char *))
{
	QList<DiveComputerNode> values = dcList.dcMap.values();
	qSort(values.begin(), values.end(), compareDC);
	for (int i = 0; i < values.size(); i++) {
		const DiveComputerNode *node = &values.at(i);
		callback(f, node->model.toUtf8().data(), node->deviceId, node->nickName.toUtf8().data(),
			 node->serialNumber.toUtf8().data(), node->firmware.toUtf8().data());
	}
}

int gettimezoneoffset()
{
	QDateTime dt1 = QDateTime::currentDateTime();
	QDateTime dt2 = dt1.toUTC();
	dt1.setTimeSpec(Qt::UTC);
	return dt2.secsTo(dt1);
}

int parseTemperatureToMkelvin(const QString& text)
{
	int mkelvin;
	QString numOnly = text;
	numOnly.replace(",",".").remove(QRegExp("[^-0-9.]"));
	if (numOnly == "")
		return 0;
	double number = numOnly.toDouble();
	switch (prefs.units.temperature) {
	case units::CELSIUS:
		mkelvin = C_to_mkelvin(number);
		break;
	case units::FAHRENHEIT:
		mkelvin = F_to_mkelvin(number);
		break;
	default:
		mkelvin = 0;
	}
	return mkelvin;

}

QString get_dive_date_string(timestamp_t when)
{
	struct tm tm;
	utc_mkdate(when, &tm);
	return translate("gettextFromC", "%1, %2 %3, %4 %5:%6")
		.arg(weekday(tm.tm_wday))
		.arg(monthname(tm.tm_mon))
		.arg(tm.tm_mday)
		.arg(tm.tm_year + 1900)
		.arg(tm.tm_hour, 2, 10, QChar('0'))
		.arg(tm.tm_min, 2, 10, QChar('0'));
}

QString get_short_dive_date_string(timestamp_t when)
{
	struct tm tm;
	utc_mkdate(when, &tm);
	return translate("gettextFromC", "%1 %2, %3\n%4:%5")
		.arg(monthname(tm.tm_mon))
		.arg(tm.tm_mday)
		.arg(tm.tm_year + 1900)
		.arg(tm.tm_hour, 2, 10, QChar('0'))
		.arg(tm.tm_min, 2, 10, QChar('0'));
}

QString get_trip_date_string(timestamp_t when, int nr)
{
	struct tm tm;
	utc_mkdate(when, &tm);
	if (nr != 1)
		return translate("gettextFromC", "%1 %2 (%3 dives)")
			.arg(monthname(tm.tm_mon))
			.arg(tm.tm_year + 1900)
			.arg(nr);
	else
		return translate("gettextFromC", "%1 %2 (1 dive)")
			.arg(monthname(tm.tm_mon))
			.arg(tm.tm_year + 1900);
}

static xmlDocPtr get_stylesheet_doc(const xmlChar *uri, xmlDictPtr, int, void *, xsltLoadType)
{
	QFile f(QLatin1String(":/xslt/") + (const char *)uri);
	if (!f.open(QIODevice::ReadOnly))
		return NULL;

	/* Load and parse the data */
	QByteArray source = f.readAll();

	xmlDocPtr doc = xmlParseMemory(source, source.size());
	return doc;
}

xsltStylesheetPtr get_stylesheet(const char *name)
{
	// this needs to be done only once, but doesn't hurt to run every time
	xsltSetLoaderFunc(get_stylesheet_doc);

	// get main document:
	xmlDocPtr doc = get_stylesheet_doc((const xmlChar *)name, NULL, 0, NULL, XSLT_LOAD_START);
	if (!doc)
		return NULL;

//	xsltSetGenericErrorFunc(stderr, NULL);
	xsltStylesheetPtr xslt = xsltParseStylesheetDoc(doc);
	if (!xslt) {
		xmlFreeDoc(doc);
		return NULL;
	}

	return xslt;
}
