#include "profilegraphics.h"
#include "mainwindow.h"
#include "divelistview.h"

#include <QGraphicsScene>
#include <QResizeEvent>
#include <QGraphicsLineItem>
#include <QScrollBar>
#include <QPen>
#include <QBrush>
#include <QDebug>
#include <QLineF>
#include <QSettings>
#include <QIcon>
#include <QPropertyAnimation>
#include <QGraphicsSceneHoverEvent>
#include <QMouseEvent>
#include <QToolBar>
#include <qtextdocument.h>
#include <QMessageBox>
#include <limits>

#include "../color.h"
#include "../display.h"
#include "../dive.h"
#include "../profile.h"
#include "../device.h"
#include "../helpers.h"
#include "../planner.h"
#include "../gettextfromc.h"

#include <libdivecomputer/parser.h>
#include <libdivecomputer/version.h>

static struct graphics_context last_gc;

#if PRINT_IMPLEMENTED
static double plot_scale = SCALE_SCREEN;
#endif

struct text_render_options {
	double size;
	color_indice_t color;
	double hpos, vpos;
};

extern struct ev_select *ev_namelist;
extern int evn_allocated;
extern int evn_used;

#define TOOLBAR_POS \
QPoint(viewport()->geometry().width() - toolBarProxy->boundingRect().width(), \
	viewport()->geometry().height() - toolBarProxy->boundingRect().height() )

ProfileGraphicsView::ProfileGraphicsView(QWidget* parent) : QGraphicsView(parent),
	toolTip(0) , diveId(0), diveDC(0), rulerItem(0), toolBarProxy(0)
{
	printMode = false;
	isGrayscale = false;
	rulerEnabled = false;
	gc.printer = false;
	fill_profile_color();
	setScene(new QGraphicsScene(this));

	scene()->installEventFilter(this);

	setRenderHint(QPainter::Antialiasing);
	setRenderHint(QPainter::HighQualityAntialiasing);
	setRenderHint(QPainter::SmoothPixmapTransform);

	defaultPen.setJoinStyle(Qt::RoundJoin);
	defaultPen.setCapStyle(Qt::RoundCap);
	defaultPen.setWidth(2);
	defaultPen.setCosmetic(true);

	setHorizontalScrollBarPolicy (Qt::ScrollBarAlwaysOff);
	setVerticalScrollBarPolicy (Qt::ScrollBarAlwaysOff);
}

/* since we cannot use translate() directly on the scene we hack on
 * the scroll bars (hidden) functionality */
void ProfileGraphicsView::scrollViewTo(const QPoint pos)
{
	if (!zoomLevel)
		return;
	QScrollBar *vs = verticalScrollBar();
	QScrollBar *hs = horizontalScrollBar();
	const qreal yRat = pos.y() / sceneRect().height();
	const qreal xRat = pos.x() / sceneRect().width();
	const int vMax = vs->maximum();
	const int hMax = hs->maximum();
	const int vMin = vs->minimum();
	const int hMin = hs->minimum();
	/* QScrollBar receives crazy negative values for minimum */
	vs->setValue(yRat * (vMax - vMin) + vMin * 0.9);
	hs->setValue(xRat * (hMax - hMin) + hMin * 0.9);
}

void ProfileGraphicsView::wheelEvent(QWheelEvent* event)
{
	if (!toolTip)
		return;

	// doesn't seem to work for Qt 4.8.1
	// setTransformationAnchor(QGraphicsView::AnchorUnderMouse);

	// Scale the view / do the zoom
	QPoint toolTipPos = mapFromScene(toolTip->pos());

	double scaleFactor = 1.15;
	if (event->delta() > 0 && zoomLevel < 20) {
		scale(scaleFactor, scaleFactor);
		zoomLevel++;
	} else if (event->delta() < 0 && zoomLevel > 0) {
		// Zooming out
		scale(1.0 / scaleFactor, 1.0 / scaleFactor);
		zoomLevel--;
	}

	scrollViewTo(event->pos());
	toolTip->setPos(mapToScene(toolTipPos));
	toolBarProxy->setPos(mapToScene(TOOLBAR_POS));
	if (zoomLevel != 0) {
		toolBarProxy->hide();
	} else {
		toolBarProxy->show();
	}
}

void ProfileGraphicsView::contextMenuEvent(QContextMenuEvent* event)
{
	if (selected_dive == -1)
		return;
	QMenu m;
	QMenu *gasChange = m.addMenu(tr("Add Gas Change"));
	GasSelectionModel *model = GasSelectionModel::instance();
	model->repopulate();
	int rowCount = model->rowCount();
	for (int i = 0; i < rowCount; i++) {
		QAction *action = new QAction(&m);
		action->setText( model->data(model->index(i, 0),Qt::DisplayRole).toString());
		connect(action, SIGNAL(triggered(bool)), this, SLOT(changeGas()));
		action->setData(event->globalPos());
		gasChange->addAction(action);
	}
	QAction *action = m.addAction(tr("Add Bookmark"), this, SLOT(addBookmark()));
	action->setData(event->globalPos());
	QList<QGraphicsItem*> itemsAtPos = scene()->items(mapToScene(mapFromGlobal(event->globalPos())));
	Q_FOREACH(QGraphicsItem *i, itemsAtPos) {
		EventItem *item = dynamic_cast<EventItem*>(i);
		if (!item)
			continue;
		QAction *action = new QAction(&m);
		action->setText(tr("Remove Event"));
		action->setData(QVariant::fromValue<void*>(item)); // so we know what to remove.
		connect(action, SIGNAL(triggered(bool)), this, SLOT(removeEvent()));
		m.addAction(action);
		action = new QAction(&m);
		action->setText(tr("Hide similar events"));
		action->setData(QVariant::fromValue<void*>(item));
		connect(action, SIGNAL(triggered(bool)), this, SLOT(hideEvents()));
		m.addAction(action);
		break;
	}
	bool some_hidden = false;
	for (int i = 0; i < evn_used; i++) {
		if (ev_namelist[i].plot_ev == false) {
			some_hidden = true;
			break;
		}
	}
	if (some_hidden) {
		action = m.addAction(tr("Unhide all events"), this, SLOT(unhideEvents()));
		action->setData(event->globalPos());
	}
	m.exec(event->globalPos());
}

void ProfileGraphicsView::addBookmark()
{
	QAction *action = qobject_cast<QAction*>(sender());
	QPoint globalPos = action->data().toPoint();
	QPoint viewPos = mapFromGlobal(globalPos);
	QPointF scenePos = mapToScene(viewPos);
	int seconds = scenePos.x() / gc.maxx * (gc.rightx - gc.leftx) + gc.leftx;
	add_event(current_dc, seconds, SAMPLE_EVENT_BOOKMARK, 0, 0, "bookmark");
	mark_divelist_changed(true);
	plot(current_dive, true);
}

void ProfileGraphicsView::changeGas()
{
	QAction *action = qobject_cast<QAction*>(sender());
	QPoint globalPos = action->data().toPoint();
	QPoint viewPos = mapFromGlobal(globalPos);
	QPointF scenePos = mapToScene(viewPos);
	QString gas = action->text();
	int o2, he;
	validate_gas(gas.toUtf8().constData(), &o2, &he);
	int seconds = scenePos.x() / gc.maxx * (gc.rightx - gc.leftx) + gc.leftx;
	add_gas_switch_event(current_dive, current_dc, seconds, get_gasidx(current_dive, o2, he));
	// this means we potentially have a new tank that is being used and needs to be shown
	fixup_dive(current_dive);
	mainWindow()->information()->updateDiveInfo(selected_dive);
	mark_divelist_changed(true);
	plot(current_dive, true);
}

void ProfileGraphicsView::hideEvents()
{
	QAction *action = qobject_cast<QAction*>(sender());
	EventItem *item = static_cast<EventItem*>(action->data().value<void*>());
	struct event *event = item->ev;

	if (QMessageBox::question(mainWindow(), TITLE_OR_TEXT(
				  tr("Hide events"),
				  tr("Hide all %1 events?").arg(event->name)),
				  QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {
		if (event->name) {
			for (int i = 0; i < evn_used; i++) {
				if (! strcmp(event->name, ev_namelist[i].ev_name)) {
					ev_namelist[i].plot_ev = false;
					break;
				}
			}
		}
		plot(current_dive, true);
	}
}

void ProfileGraphicsView::unhideEvents()
{
	for (int i = 0; i < evn_used; i++) {
		ev_namelist[i].plot_ev = true;
	}
	plot(current_dive, true);
}

void ProfileGraphicsView::removeEvent()
{
	QAction *action = qobject_cast<QAction*>(sender());
	EventItem *item = static_cast<EventItem*>(action->data().value<void*>());
	struct event *event = item->ev;

	if (QMessageBox::question(mainWindow(), TITLE_OR_TEXT(
				  tr("Remove the selected event?"),
				  tr("%1 @ %2:%3").arg(event->name)
				  .arg(event->time.seconds / 60)
				  .arg(event->time.seconds % 60, 2, 10, QChar('0'))),
				  QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {
		struct event **ep = &current_dc->events;
		while (ep && *ep != event)
			ep = &(*ep)->next;
		if (ep) {
			*ep = event->next;
			free(event);
		}
		mark_divelist_changed(true);
		plot(current_dive, true);
	}
}


void ProfileGraphicsView::mouseMoveEvent(QMouseEvent* event)
{
	if (!toolTip)
		return;

	toolTip->refresh(&gc,  mapToScene(event->pos()));
	QPoint toolTipPos = mapFromScene(toolTip->pos());
	scrollViewTo(event->pos());

	if (zoomLevel == 0) {
		QGraphicsView::mouseMoveEvent(event);
	} else {
		toolTip->setPos(mapToScene(toolTipPos));
		toolBarProxy->setPos(mapToScene(TOOLBAR_POS));
	}
}

bool ProfileGraphicsView::eventFilter(QObject* obj, QEvent* event)
{
	if (event->type() == QEvent::Leave) {
		if (toolTip && toolTip->isExpanded())
			toolTip->collapse();
		return true;
	}

	// This will "Eat" the default tooltip behavior if it is not on the toolBar.
	if (event->type() == QEvent::GraphicsSceneHelp) {
		if (toolBarProxy && !toolBarProxy->geometry().contains(mapToScene(mapFromGlobal(QCursor::pos())))) {
			event->ignore();
			return true;
		}
	}
	return QGraphicsView::eventFilter(obj, event);
}

#if PRINT_IMPLEMENTED
static void plot_set_scale(scale_mode_t scale)
{
	switch (scale) {
	default:
	case SC_SCREEN:
		plot_scale = SCALE_SCREEN;
		break;
	case SC_PRINT:
		plot_scale = SCALE_PRINT;
		break;
	}
}
#endif

void ProfileGraphicsView::showEvent(QShowEvent* event)
{
	// Program just opened,
	// but the dive was not ploted.
	// force a replot by modifying the dive
	// hold by the view, and issuing a plot.
	if (diveId && !scene()->items().count()) {
		diveId = 0;
		plot(get_dive(selected_dive));
	}
	if (toolBarProxy)
		toolBarProxy->setPos(mapToScene(TOOLBAR_POS));
}

void ProfileGraphicsView::clear()
{
	resetTransform();
	zoomLevel = 0;
	if (toolTip) {
		scene()->removeItem(toolTip);
		toolTip->deleteLater();
		toolTip = 0;
	}
	if (toolBarProxy) {
		scene()->removeItem(toolBarProxy);
		toolBarProxy->deleteLater();
		toolBarProxy = 0;
	}
	if (rulerItem) {
		remove_ruler();
		rulerItem->destNode()->deleteLater();
		rulerItem->sourceNode()->deleteLater();
		rulerItem->deleteLater();
		rulerItem=0;
	}
	scene()->clear();
}

void ProfileGraphicsView::refresh()
{
	clear();
	plot(current_dive, true);
}

void ProfileGraphicsView::setPrintMode(bool mode, bool grayscale)
{
	printMode = mode;
	isGrayscale = grayscale;
}

QColor ProfileGraphicsView::getColor(const color_indice_t i)
{
	return profile_color[i].at((isGrayscale) ? 1 : 0);
}

void ProfileGraphicsView::plot(struct dive *d, bool forceRedraw)
{
	struct divecomputer *dc = NULL;

	if (d)
		dc = select_dc(&d->dc);

	if (!forceRedraw && getDiveById(diveId) == d && (d && dc == diveDC))
		return;

	clear();
	diveId = d ? d->id : 0;
	diveDC = d ? dc : NULL;

	if (!isVisible() || !d || !mainWindow()) {
		return;
	}
	setBackgroundBrush(getColor(BACKGROUND));

	// best place to put the focus stealer code.
	setFocusProxy(mainWindow()->dive_list());
	scene()->setSceneRect(0,0, viewport()->width()-50, viewport()->height()-50);

	toolTip = new ToolTipItem();
	installEventFilter(toolTip);
	scene()->addItem(toolTip);
	if (printMode)
		toolTip->setVisible(false);

	// Fix this for printing / screen later.
	// plot_set_scale(scale_mode_t);

	if (!dc || !dc->samples) {
		dc = fake_dc(dc);
	}

	QString nick = get_dc_nickname(dc->model, dc->deviceid);
	if (nick.isEmpty())
		nick = QString(dc->model);

	if (nick.isEmpty())
		nick = tr("unknown divecomputer");

	if ( tr("unknown divecomputer") == nick) {
		mode = PLAN;
	} else {
		mode = DIVE;
	}

	/*
	 * Set up limits that are independent of
	 * the dive computer
	 */
	calculate_max_limits(d, dc, &gc);

	QRectF profile_grid_area = scene()->sceneRect();
	gc.maxx = (profile_grid_area.width() - 2 * profile_grid_area.x());
	gc.maxy = (profile_grid_area.height() - 2 * profile_grid_area.y());

	/* This is per-dive-computer */
	gc.pi = *create_plot_info(d, dc, &gc, printMode);

	/* Bounding box */
	QPen pen = defaultPen;
	pen.setColor(getColor(TIME_GRID));
	QGraphicsRectItem *rect = new QGraphicsRectItem(profile_grid_area);
	rect->setPen(pen);
	scene()->addItem(rect);

	/* Depth profile */
	plot_depth_profile();
	plot_events(dc);

	if (rulerEnabled && !printMode)
		create_ruler();

	/* Temperature profile */
	plot_temperature_profile();

	/* Cylinder pressure plot */
	plot_cylinder_pressure();

	/* Text on top of all graphs.. */
	plot_temperature_text();
	plot_depth_text();
	plot_cylinder_pressure_text();
	plot_deco_text();

	/* Put the dive computer name in the lower left corner */
	gc.leftx = 0; gc.rightx = 1.0;
	gc.topy = 0; gc.bottomy = 1.0;

	text_render_options_t computer = {DC_TEXT_SIZE, TIME_TEXT, LEFT, TOP};
	diveComputer = plot_text(&computer, QPointF(gc.leftx, gc.bottomy), nick);
	// The Time ruler should be right after the DiveComputer:
	timeMarkers->setPos(0, diveComputer->y());

	if (PP_GRAPHS_ENABLED) {
		plot_pp_gas_profile();
		plot_pp_text();
	}

	plot_depth_scale();

#if 0
	if (gc->printer) {
		free(pi->entry);
		last_pi_entry = pi->entry = NULL;
		pi->nr = 0;
	}
#endif

	QRectF r = scene()->itemsBoundingRect();
	scene()->setSceneRect(r.x() - 15, r.y() -15, r.width() + 30, r.height() + 30);
	if (zoomLevel == 0) {
		fitInView(sceneRect());
	}
	toolTip->readPos();

	if (mode == PLAN) {
		timeEditor = new GraphicsTextEditor();
		timeEditor->setPlainText(d->duration.seconds ? QString::number(d->duration.seconds/60) : tr("Set Duration: 10 minutes"));
		timeEditor->setPos(profile_grid_area.width() - timeEditor->boundingRect().width(), timeMarkers->y());
		timeEditor->document();
		connect(timeEditor, SIGNAL(editingFinished(QString)), this, SLOT(edit_dive_time(QString)));
		scene()->addItem(timeEditor);
	}

	if (!printMode)
		addControlItems(d);

	if (rulerEnabled && !printMode)
		add_ruler();

	gc.rightx = get_maxtime(&gc.pi);
}

void ProfileGraphicsView::plot_depth_scale()
{
	int i, maxdepth, marker;
	static text_render_options_t tro = {DEPTH_TEXT_SIZE, SAMPLE_DEEP, RIGHT, MIDDLE};

	/* Depth markers: every 30 ft or 10 m*/
	maxdepth = get_maxdepth(&gc.pi);
	gc.topy = 0; gc.bottomy = maxdepth;
	marker = M_OR_FT(10,30);

	/* don't write depth labels all the way to the bottom as
	 * there may be other graphs below the depth plot (like
	 * partial pressure graphs) where this would look out
	 * of place - so we only make sure that we print the next
	 * marker below the actual maxdepth of the dive */
	depthMarkers = new QGraphicsRectItem();
	for (i = marker; i <= gc.pi.maxdepth + marker; i += marker) {
		double d = get_depth_units(i, NULL, NULL);
		plot_text(&tro, QPointF(-0.002, i), QString::number(d), depthMarkers);
	}
	scene()->addItem(depthMarkers);
	depthMarkers->setPos(depthMarkers->pos().x() - 10, 0);
}

void ProfileGraphicsView::addControlItems(struct dive *d)
{
	QAction *scaleAction = new QAction(QIcon(":scale"), tr("Scale"), this);
	QAction *rulerAction = new QAction(QIcon(":ruler"), tr("Ruler"), this);
	QToolBar *toolBar = new QToolBar("", 0);
	rulerAction->setToolTip(tr("Measure properties of dive segments"));
	scaleAction->setToolTip(tr("Scale your dive to screen size"));
	toolBar->addAction(rulerAction);
	toolBar->addAction(scaleAction);
	toolBar->setOrientation(Qt::Vertical);
	//make toolbar transparent
	//toolBar->setStyleSheet(QString::fromUtf8 ("background-color: rgba(255,255,255,0);"));

	connect(scaleAction, SIGNAL(triggered()), this, SLOT(on_scaleAction()));
	connect(rulerAction, SIGNAL(triggered()), this, SLOT(on_rulerAction()));
	//Put it into the lower right corner of the profile

	QString defaultDC(d->dc.model);
	if (defaultDC == "manually added dive" || defaultDC == "planned dive") {
		QAction *editAction = new QAction(QIcon(":edit"), tr("Edit"), this);
		toolBar->addAction(editAction);
		connect(editAction, SIGNAL(triggered()), mainWindow(), SLOT(editCurrentDive()));
	}
	toolBarProxy = scene()->addWidget(toolBar);
	toolBarProxy->setPos(mapToScene(TOOLBAR_POS));
	toolBarProxy->setFlag(QGraphicsItem::ItemIgnoresTransformations);
}

void ProfileGraphicsView::plot_pp_text()
{
	double pp, dpp, m;
	int hpos;
	static text_render_options_t tro = {PP_TEXT_SIZE, PP_LINES, LEFT, -0.75};
	QGraphicsRectItem *pressureMarkers = new QGraphicsRectItem();

	setup_pp_limits(&gc);
	pp = floor(gc.pi.maxpp * 10.0) / 10.0 + 0.2;
	dpp = pp > 4 ? 0.5 : 0.25;
	hpos = gc.pi.entry[gc.pi.nr - 1].sec;
	QColor c = getColor(PP_LINES);

	bool alt = true;
	for (m = 0.0; m <= pp; m += dpp) {
		QGraphicsLineItem *item = new QGraphicsLineItem(SCALEGC(0, m), SCALEGC(hpos, m));
		QPen pen(defaultPen);
		pen.setColor(c);
		if ( QString::number(m).toDouble() != QString::number(m).toInt()) {
			pen.setStyle(Qt::DashLine);
			pen.setWidthF(1.2);
		}
		item->setPen(pen);
		scene()->addItem(item);
		qreal textPos = hpos;
		if (alt)
			plot_text(&tro, QPointF(textPos, m), QString::number(m), pressureMarkers);
		alt = !alt;
	}
	scene()->addItem(pressureMarkers);
	pressureMarkers->setPos(pressureMarkers->pos().x() + 10, 0);
}

void ProfileGraphicsView::plot_add_line(int sec, double val, QColor c, QPointF &from)
{
	QPointF to = QPointF(SCALEGC(sec, val));
	QGraphicsLineItem *item = new QGraphicsLineItem(from.x(), from.y(), to.x(), to.y());
	QPen pen(defaultPen);
	pen.setColor(c);
	item->setPen(pen);
	scene()->addItem(item);
	from = to;
}

void ProfileGraphicsView::plot_pp_gas_profile()
{
	int i;
	struct plot_data *entry;
	struct plot_info *pi = &gc.pi;

	setup_pp_limits(&gc);
	QColor c;
	QPointF from, to;
	QPointF legendPos = QPointF(scene()->sceneRect().width() * 0.4, scene()->sceneRect().height() - scene()->sceneRect().height()*0.02);

	if (prefs.pp_graphs.pn2) {
		c = getColor(PN2);
		entry = pi->entry;
		from = QPointF(SCALEGC(entry->sec, entry->pn2));
		for (i = 1; i < pi->nr; i++) {
			entry++;
			if (entry->pn2 < prefs.pp_graphs.pn2_threshold)
				plot_add_line(entry->sec, entry->pn2, c, from);
			else
				from = QPointF(SCALEGC(entry->sec, entry->pn2));
		}

		c = getColor(PN2_ALERT);
		entry = pi->entry;
		from = QPointF(SCALEGC(entry->sec, entry->pn2));
		for (i = 1; i < pi->nr; i++) {
			entry++;
			if (entry->pn2 >= prefs.pp_graphs.pn2_threshold)
				plot_add_line(entry->sec, entry->pn2, c, from);
			else
				from = QPointF(SCALEGC(entry->sec, entry->pn2));
		}
		createPPLegend(trUtf8("pN" UTF8_SUBSCRIPT_2),getColor(PN2), legendPos);
	}

	if (prefs.pp_graphs.phe) {
		c = getColor(PHE);
		entry = pi->entry;

		from = QPointF(SCALEGC(entry->sec, entry->phe));
		for (i = 1; i < pi->nr; i++) {
			entry++;
			if (entry->phe < prefs.pp_graphs.phe_threshold)
				plot_add_line(entry->sec, entry->phe, c, from);
			else
				from = QPointF(SCALEGC(entry->sec, entry->phe));
		}

		c = getColor(PHE_ALERT);
		entry = pi->entry;
		from = QPointF(SCALEGC(entry->sec, entry->phe));
		for (i = 1; i < pi->nr; i++) {
			entry++;
			if (entry->phe >= prefs.pp_graphs.phe_threshold)
				plot_add_line(entry->sec, entry->phe, c, from);
			else
				from = QPointF(SCALEGC(entry->sec, entry->phe));
		}
		createPPLegend(trUtf8("pHe"),getColor(PHE), legendPos);
	}
	if (prefs.pp_graphs.po2) {
		c = getColor(PO2);
		entry = pi->entry;
		from = QPointF(SCALEGC(entry->sec, entry->po2));
		for (i = 1; i < pi->nr; i++) {
			entry++;
			if (entry->po2 < prefs.pp_graphs.po2_threshold)
				plot_add_line(entry->sec, entry->po2, c, from);
			else
				from = QPointF(SCALEGC(entry->sec, entry->po2));
		}

		c = getColor(PO2_ALERT);
		entry = pi->entry;
		from = QPointF(SCALEGC(entry->sec, entry->po2));
		for (i = 1; i < pi->nr; i++) {
			entry++;
			if (entry->po2 >= prefs.pp_graphs.po2_threshold)
				plot_add_line(entry->sec, entry->po2, c, from);
			 else
				from = QPointF(SCALEGC(entry->sec, entry->po2));
		}
		createPPLegend(trUtf8("pO" UTF8_SUBSCRIPT_2),getColor(PO2), legendPos);
	}
}

void ProfileGraphicsView::createPPLegend(QString title, const QColor& c, QPointF& legendPos)
{
	QGraphicsRectItem *rect = new QGraphicsRectItem(0, 0, scene()->sceneRect().width() * 0.01, scene()->sceneRect().width() * 0.01);
	rect->setBrush(QBrush(c));
	rect->setPos(legendPos);
	rect->setPen(QPen(QColor(Qt::transparent)));
	QGraphicsSimpleTextItem *text = new QGraphicsSimpleTextItem(title);
	text->setPos(legendPos.x() + rect->boundingRect().width() + 5, legendPos.y() );
	scene()->addItem(rect);
	scene()->addItem(text);
	legendPos.setX(text->pos().x() + text->boundingRect().width() + 20);
	if (printMode) {
		QFont f = text->font();
		f.setPointSizeF( f.pointSizeF() * 0.7);
		text->setFont(f);
	}
}

void ProfileGraphicsView::plot_deco_text()
{
	if (prefs.profile_calc_ceiling) {
		float x = gc.leftx + (gc.rightx - gc.leftx) / 2;
		float y = gc.topy = 1.0;
		static text_render_options_t tro = {PRESSURE_TEXT_SIZE, PRESSURE_TEXT, CENTER, BOTTOM};
		gc.bottomy = 0.0;
		plot_text(&tro, QPointF(x, y), QString("GF %1/%2").arg(prefs.gflow).arg(prefs.gfhigh));
	}
}

void ProfileGraphicsView::plot_cylinder_pressure_text()
{
	int i;
	int mbar, cyl;
	int seen_cyl[MAX_CYLINDERS] = { false, };
	int last_pressure[MAX_CYLINDERS] = { 0, };
	int last_time[MAX_CYLINDERS] = { 0, };
	struct plot_data *entry;
	struct plot_info *pi = &gc.pi;
	struct dive *dive = getDiveById(diveId);
	Q_ASSERT(dive != NULL);

	if (!get_cylinder_pressure_range(&gc))
		return;

	for (i = 0; i < pi->nr; i++) {
		entry = pi->entry + i;
		for (cyl = 0; cyl < MAX_CYLINDERS; cyl++) {
			mbar = GET_PRESSURE(entry->cylinder[cyl]);

			if (!mbar)
				continue;
			if (entry->cylinder[cyl].usage != NOT_IN_USE) {
				if (!seen_cyl[cyl]) {
					plot_pressure_value(mbar, entry->sec, LEFT, BOTTOM);
					plot_gas_value(mbar, entry->sec, LEFT, TOP,
							get_o2(&dive->cylinder[cyl].gasmix),
							get_he(&dive->cylinder[cyl].gasmix));
					seen_cyl[cyl] = true;
				}
			}
			last_pressure[cyl] = mbar;
			last_time[cyl] = entry->sec;
		}
	}

	for (cyl = 0; cyl < MAX_CYLINDERS; cyl++) {
		if (last_time[cyl]) {
			plot_pressure_value(last_pressure[cyl], last_time[cyl], CENTER, TOP);
		}
	}
}

void ProfileGraphicsView::plot_pressure_value(int mbar, int sec, double xalign, double yalign)
{
	int pressure;
	const char *unit;

	pressure = get_pressure_units(mbar, &unit);
	static text_render_options_t tro = {PRESSURE_TEXT_SIZE, PRESSURE_TEXT, xalign, yalign};
	plot_text(&tro, QPointF(sec, mbar), QString("%1 %2").arg(pressure).arg(unit));
}

void ProfileGraphicsView::plot_gas_value(int mbar, int sec, double xalign, double yalign, int o2, int he)
{
	QString gas;
	if (is_air(o2, he))
		gas = tr("air");
	else if (he == 0)
		gas = QString(tr("EAN%1")).arg((o2 + 5) / 10);
	else
		gas = QString("%1/%2").arg((o2 + 5) / 10).arg((he + 5) / 10);
	static text_render_options_t tro = {PRESSURE_TEXT_SIZE, PRESSURE_TEXT, xalign, yalign};
	plot_text(&tro, QPointF(sec, mbar), gas);

}

void ProfileGraphicsView::plot_depth_text()
{
	int maxtime, maxdepth;

	/* Get plot scaling limits */
	maxtime = get_maxtime(&gc.pi);
	maxdepth = get_maxdepth(&gc.pi);

	gc.leftx = 0; gc.rightx = maxtime;
	gc.topy = 0; gc.bottomy = maxdepth;

	plot_text_samples();
}

void ProfileGraphicsView::plot_text_samples()
{
	static text_render_options_t deep = {14, SAMPLE_DEEP, CENTER, TOP};
	static text_render_options_t shallow = {14, SAMPLE_SHALLOW, CENTER, BOTTOM};
	int i;
	int last = -1;

	struct plot_info* pi = &gc.pi;

	for (i = 0; i < pi->nr; i++) {
		struct plot_data *entry = pi->entry + i;

		if (entry->depth < 2000)
			continue;

		if ((entry == entry->max[2]) && entry->depth / 100 != last) {
			plot_depth_sample(entry, &deep);
			last = entry->depth / 100;
		}

		if ((entry == entry->min[2]) && entry->depth / 100 != last) {
			plot_depth_sample(entry, &shallow);
			last = entry->depth / 100;
		}

		if (entry->depth != last)
			last = -1;
	}
}

void ProfileGraphicsView::plot_depth_sample(struct plot_data *entry,text_render_options_t *tro)
{
	int sec = entry->sec, decimals;
	double d;

	d = get_depth_units(entry->depth, &decimals, NULL);

	plot_text(tro, QPointF(sec, entry->depth), QString("%1").arg(d, 0, 'f', 1));
}

void ProfileGraphicsView::plot_temperature_text()
{
	int i;
	int last = -300, sec = 0;
	int last_temperature = 0, last_printed_temp = 0;
	plot_info *pi = &gc.pi;

	if (!setup_temperature_limits(&gc))
		return;

	for (i = 0; i < pi->nr; i++) {
		struct plot_data *entry = pi->entry+i;
		int mkelvin = entry->temperature;
		sec = entry->sec;

		if (!mkelvin)
			continue;
		last_temperature = mkelvin;
		/* don't print a temperature
		 * if it's been less than 5min and less than a 2K change OR
		 * if it's been less than 2min OR if the change from the
		 * last print is less than .4K (and therefore less than 1F) */
		if (((sec < last + 300) && (abs(mkelvin - last_printed_temp) < 2000)) ||
		    (sec < last + 120) ||
		    (abs(mkelvin - last_printed_temp) < 400))
			continue;
		last = sec;
		if (mkelvin > 200000)
			plot_single_temp_text(sec,mkelvin);
		last_printed_temp = mkelvin;
	}
	/* it would be nice to print the end temperature, if it's
	 * different or if the last temperature print has been more
	 * than a quarter of the dive back */
	if (last_temperature > 200000 &&
	    ((abs(last_temperature - last_printed_temp) > 500) || ((double)last / (double)sec < 0.75)))
		plot_single_temp_text(sec, last_temperature);
}

void ProfileGraphicsView::plot_single_temp_text(int sec, int mkelvin)
{
	double deg;
	const char *unit;
	static text_render_options_t tro = {TEMP_TEXT_SIZE, TEMP_TEXT, LEFT, TOP};
	deg = get_temp_units(mkelvin, &unit);
	plot_text(&tro, QPointF(sec, mkelvin), QString("%1%2").arg(deg, 0, 'f', 1).arg(unit)); //"%.2g%s"
}

void ProfileGraphicsView::plot_cylinder_pressure()
{
	int i, cyl;
	int lift_pen = false;
	int first_plot = true;
	cylinder_segment_use_t last_usage = NOT_IN_USE;

	if (!get_cylinder_pressure_range(&gc))
		return;

	struct dive *dive = getDiveById(diveId);
	Q_ASSERT(dive != NULL);
	QPointF from, to;
	for (i = 0; i < gc.pi.nr; i++) {
		int mbar;
		struct plot_data *entry = gc.pi.entry + i;
		for (cyl = 0; cyl < MAX_CYLINDERS; cyl++) {
			mbar = GET_PRESSURE(entry->cylinder[cyl]);
			if (entry->cylinder[cyl].usage == NOT_IN_USE) {
				lift_pen = true;
			}
			if (!mbar) {
				lift_pen = true;
				continue;
			}

			QColor c = get_sac_color(entry->cylinder[cyl].sac, dive->sac);

			if (lift_pen) {
				if (!first_plot && entry->cylinder[cyl].usage == last_usage) {
					/* if we have a previous event from the same tank,
					 * draw at least a short line */
					int prev_pr;
					prev_pr = GET_PRESSURE((entry - 1)->cylinder[cyl]);

					QGraphicsLineItem *item = new QGraphicsLineItem(SCALEGC((entry-1)->sec, prev_pr), SCALEGC(entry->sec, mbar));
					QPen pen(defaultPen);
					pen.setColor(c);
					item->setPen(pen);
					scene()->addItem(item);
				} else {
					first_plot = false;
					from = QPointF(SCALEGC(entry->sec, mbar));
				}
				lift_pen = false;
			} else {
				to = QPointF(SCALEGC(entry->sec, mbar));
				QGraphicsLineItem *item = new QGraphicsLineItem(from.x(), from.y(), to.x(), to.y());
				QPen pen(defaultPen);
				pen.setColor(c);
				item->setPen(pen);
				scene()->addItem(item);
			}

			from = QPointF(SCALEGC(entry->sec, mbar));
			last_usage = entry->cylinder[cyl].usage;
		}
	}
}

/* set the color for the pressure plot according to temporary sac rate
 * as compared to avg_sac; the calculation simply maps the delta between
 * sac and avg_sac to indexes 0 .. (SAC_COLORS - 1) with everything
 * more than 6000 ml/min below avg_sac mapped to 0 */
QColor ProfileGraphicsView::get_sac_color(int sac, int avg_sac)
{
	int sac_index = 0;
	int delta = sac - avg_sac + 7000;

	if (!gc.printer) {
		sac_index = delta / 2000;
		if (sac_index < 0)
			sac_index = 0;
		if (sac_index > SAC_COLORS - 1)
			sac_index = SAC_COLORS - 1;
		return getColor((color_indice_t)(SAC_COLORS_START_IDX + sac_index));
	}
	return getColor(SAC_DEFAULT);
}

void ProfileGraphicsView::plot_events(struct divecomputer *dc)
{
	struct event *event = dc->events;

//	if (gc->printer) {
//		return;
//	}

	while (event) {
		plot_one_event(event);
		event = event->next;
	}
}

void ProfileGraphicsView::plot_one_event(struct event *ev)
{
	int i;
	struct plot_info *pi = &gc.pi;
	struct plot_data *entry = NULL;

	/* is plotting of this event disabled? */
	if (ev->name) {
		for (i = 0; i < evn_used; i++) {
			if (! strcmp(ev->name, ev_namelist[i].ev_name)) {
				if (ev_namelist[i].plot_ev)
					break;
				else
					return;
			}
		}
	}

	if (ev->time.seconds < 30 && !strcmp(ev->name, "gaschange"))
		/* a gas change in the first 30 seconds is the way of some dive computers
		 * to tell us the gas that is used; let's not plot a marker for that */
		return;

	for (i = 0; i < pi->nr; i++) {
		entry = pi->entry + i;
		if (ev->time.seconds < entry->sec)
			break;
	}

	/* If we didn't find the right event, don't dereference null */
	if (entry == NULL)
		return;

	/* draw a little triangular marker and attach tooltip */

	int x = SCALEXGC(ev->time.seconds);
	int y = SCALEYGC(entry->depth);
	struct dive *dive = getDiveById(diveId);
	Q_ASSERT(dive != NULL);
	EventItem *item = new EventItem(ev, 0, isGrayscale);
	item->setPos(x, y);
	scene()->addItem(item);

	/* we display the event on screen - so translate (with the correct context for events) */
	QString name = gettextFromC::instance()->tr(ev->name);
	if (ev->value) {
		if (ev->name && strcmp(ev->name, "gaschange") == 0) {
			int cyl;
			for (cyl = 0; cyl < MAX_CYLINDERS; cyl++)
				if ((entry->cylinder[cyl].usage != OC) &&
					(entry->cylinder[cyl].usage != CCR_DILUENT))
					continue;
			if (cyl == MAX_CYLINDERS)
				return;
			int he = get_he(&dive->cylinder[cyl].gasmix);
			int o2 = get_o2(&dive->cylinder[cyl].gasmix);

			name += ": ";
			if (he)
				name += QString("%1/%2").arg((o2 + 5) / 10).arg((he + 5) / 10);
			else if (is_air(o2, he))
				name += tr("air");
			else
				name += QString(tr("EAN%1")).arg((o2 + 5) / 10);

		} else if (ev->name && !strcmp(ev->name, "SP change")) {
			name += QString(":%1").arg((double) ev->value / 1000);
		} else {
			name += QString(":%1").arg(ev->value);
		}
	} else if (ev->name && name == "SP change") {
		name += "\n" + tr("Bailing out to OC");
	} else {
		name += ev->flags == SAMPLE_FLAGS_BEGIN ? tr(" begin", "Starts with space!") :
				ev->flags == SAMPLE_FLAGS_END ? tr(" end", "Starts with space!") : "";
	}

	//item->setToolTipController(toolTip);
	//item->addToolTip(name);
	item->setToolTip(name);
}

void ProfileGraphicsView::create_ruler()
{
	int x,y;
	struct plot_info *pi = &gc.pi;
	struct plot_data *data = pi->entry;

	RulerNodeItem *first = new RulerNodeItem(0, gc);
	RulerNodeItem *second = new RulerNodeItem(0, gc);

	x = SCALEXGC(data->sec);
	y = data->depth;

	first->setPos(x,y);

	data = pi->entry+(pi->nr-1);
	x = SCALEXGC(data->sec);
	y = data->depth;

	second->setPos(x,y);
	//Make sure that both points already have their entries
	first->recalculate();
	second->recalculate();

	rulerItem = new RulerItem(0, first, second);
	first->setRuler(rulerItem);
	second->setRuler(rulerItem);
}

void ProfileGraphicsView::add_ruler()
{
	if (! scene()->items().contains(rulerItem)) {
		scene()->addItem(rulerItem->sourceNode());
		scene()->addItem(rulerItem->destNode());
		scene()->addItem(rulerItem);
		rulerItem->recalculate();
	}
}

void ProfileGraphicsView::remove_ruler()
{
	if (rulerItem) {
		if (scene()->items().contains(rulerItem))
			scene()->removeItem(rulerItem);
		if (scene()->items().contains(rulerItem->sourceNode()))
			scene()->removeItem(rulerItem->sourceNode());
		if (scene()->items().contains(rulerItem->destNode()))
			scene()->removeItem(rulerItem->destNode());
	}
}

void ProfileGraphicsView::plot_depth_profile()
{
	int i, incr;
	int sec, depth;
	struct plot_data *entry;
	int maxtime, maxdepth, marker, maxline;
	int increments[8] = { 10, 20, 30, 60, 5*60, 10*60, 15*60, 30*60 };

	/* Get plot scaling limits */
	maxtime = get_maxtime(&gc.pi);
	maxdepth = get_maxdepth(&gc.pi);

	gc.maxtime = maxtime;

	/* Time markers: at most every 10 seconds, but no more than 12 markers.
	 * We start out with 10 seconds and increment up to 30 minutes,
	 * depending on the dive time.
	 * This allows for 6h dives - enough (I hope) for even the craziest
	 * divers - but just in case, for those 8h depth-record-breaking dives,
	 * we double the interval if this still doesn't get us to 12 or fewer
	 * time markers */
	i = 0;
	while (i < 7 && maxtime / increments[i] > 12)
		i++;
	incr = increments[i];
	while (maxtime / incr > 12)
		incr *= 2;

	gc.leftx = 0; gc.rightx = maxtime;
	gc.topy = 0; gc.bottomy = 1.0;

	last_gc = gc;

	QColor c = getColor(TIME_GRID);
	for (i = incr; i < maxtime; i += incr) {
		QGraphicsLineItem *item = new QGraphicsLineItem(SCALEGC(i, 0), SCALEGC(i, 1));
		QPen pen(defaultPen);
		pen.setColor(c);
		item->setPen(pen);
		scene()->addItem(item);
	}

	timeMarkers = new QGraphicsRectItem();
	/* now the text on the time markers */
	struct text_render_options tro = {DEPTH_TEXT_SIZE, TIME_TEXT, CENTER, LINE_DOWN};
	if (maxtime < 600) {
		/* Be a bit more verbose with shorter dives */
		for (i = incr; i < maxtime; i += incr)
			plot_text(&tro, QPointF(i, 0), QString("%1:%2").arg(i/60).arg(i%60, 2, 10, QChar('0')), timeMarkers);
	} else {
		/* Only render the time on every second marker for normal dives */
		for (i = incr; i < maxtime; i += 2 * incr)
			plot_text(&tro, QPointF(i, 0), QString("%1").arg(QString::number(i/60)), timeMarkers);
	}
	timeMarkers->setPos(0,0);
	scene()->addItem(timeMarkers);

	/* Depth markers: every 30 ft or 10 m*/
	gc.leftx = 0; gc.rightx = 1.0;
	gc.topy = 0; gc.bottomy = maxdepth;
	marker = M_OR_FT(10,30);
	maxline = qMax(gc.pi.maxdepth + marker, maxdepth * 2 / 3);

	c = getColor(DEPTH_GRID);

	for (i = marker; i < maxline; i += marker) {
		QGraphicsLineItem *item = new QGraphicsLineItem(SCALEGC(0, i), SCALEGC(1, i));
		QPen pen(defaultPen);
		pen.setColor(c);
		item->setPen(pen);
		scene()->addItem(item);
	}

	gc.leftx = 0; gc.rightx = maxtime;
	c = getColor(MEAN_DEPTH);

	/* Show mean depth */
	if (! gc.printer) {
		QGraphicsLineItem *item = new QGraphicsLineItem(SCALEGC(0, gc.pi.meandepth),
								SCALEGC(gc.pi.entry[gc.pi.nr - 1].sec, gc.pi.meandepth));
		QPen pen(defaultPen);
		pen.setColor(c);
		item->setPen(pen);
		scene()->addItem(item);

		struct text_render_options tro = {DEPTH_TEXT_SIZE, MEAN_DEPTH, LEFT, TOP};
		QString depthLabel = get_depth_string(gc.pi.meandepth, true, true);
		plot_text(&tro, QPointF(gc.leftx, gc.pi.meandepth), depthLabel, item);
		tro.hpos = RIGHT;
		plot_text(&tro, QPointF(gc.pi.entry[gc.pi.nr - 1].sec, gc.pi.meandepth), depthLabel, item);
	}

#if 0
	/*
	 * These are good for debugging text placement etc,
	 * but not for actual display..
	 */
	if (0) {
		plot_smoothed_profile(gc, pi);
		plot_minmax_profile(gc, pi);
	}
#endif

	/* Do the depth profile for the neat fill */
	gc.topy = 0; gc.bottomy = maxdepth;

	entry = gc.pi.entry;

	QPolygonF p;
	QLinearGradient pat(0.0,0.0,0.0,scene()->height());
	QGraphicsPolygonItem *neatFill = NULL;

	p.append(QPointF(SCALEGC(0, 0)));
	for (i = 0; i < gc.pi.nr; i++, entry++)
		p.append(QPointF(SCALEGC(entry->sec, entry->depth)));

	/* Show any ceiling we may have encountered */
	if (prefs.profile_dc_ceiling) {
		for (i = gc.pi.nr - 1; i >= 0; i--, entry--) {
			if (!entry->in_deco) {
				/* not in deco implies this is a safety stop, no ceiling */
				p.append(QPointF(SCALEGC(entry->sec, 0)));
			} else if (entry->stopdepth < entry->depth) {
				p.append(QPointF(SCALEGC(entry->sec, entry->stopdepth)));
			} else {
				p.append(QPointF(SCALEGC(entry->sec, entry->depth)));
			}
		}
	}
	pat.setColorAt(1, getColor(DEPTH_BOTTOM));
	pat.setColorAt(0, getColor(DEPTH_TOP));

	neatFill = new QGraphicsPolygonItem();
	neatFill->setPolygon(p);
	neatFill->setBrush(QBrush(pat));
	neatFill->setPen(QPen(QBrush(Qt::transparent),0));
	scene()->addItem(neatFill);


	/* if the user wants the deco ceiling more visible, do that here (this
	 * basically draws over the background that we had allowed to shine
	 * through so far) */
	if (prefs.profile_dc_ceiling && prefs.profile_red_ceiling) {
		p.clear();
		pat.setColorAt(0, getColor(CEILING_SHALLOW));
		pat.setColorAt(1, getColor(CEILING_DEEP));

		entry = gc.pi.entry;
		p.append(QPointF(SCALEGC(0, 0)));
		for (i = 0; i < gc.pi.nr; i++, entry++) {
			if (entry->in_deco && entry->stopdepth) {
				if (entry->stopdepth < entry->depth) {
					p.append(QPointF(SCALEGC(entry->sec, entry->stopdepth)));
				} else {
					p.append(QPointF(SCALEGC(entry->sec, entry->depth)));
				}
			} else {
				p.append(QPointF(SCALEGC(entry->sec, 0)));
			}
		}

		neatFill = new QGraphicsPolygonItem();
		neatFill->setBrush(QBrush(pat));
		neatFill->setPolygon(p);
		neatFill->setPen(QPen(QBrush(Qt::NoBrush),0));
		scene()->addItem(neatFill);
	}

	/* finally, plot the calculated ceiling over all this */
	if (prefs.profile_calc_ceiling) {
		pat.setColorAt(0, getColor(CALC_CEILING_SHALLOW));
		pat.setColorAt(1, getColor(CALC_CEILING_DEEP));

		entry = gc.pi.entry;
		p.clear();
		p.append(QPointF(SCALEGC(0, 0)));
		for (i = 0; i < gc.pi.nr; i++, entry++) {
			if (entry->ceiling)
				p.append(QPointF(SCALEGC(entry->sec, entry->ceiling)));
			else
				p.append(QPointF(SCALEGC(entry->sec, 0)));
		}
		p.append(QPointF(SCALEGC((entry-1)->sec, 0)));
		neatFill = new QGraphicsPolygonItem();
		neatFill->setPolygon(p);
		neatFill->setPen(QPen(QBrush(Qt::NoBrush),0));
		neatFill->setBrush(pat);
		scene()->addItem(neatFill);
	}

	/* plot the calculated ceiling for all tissues */
	if (prefs.profile_calc_ceiling && prefs.calc_all_tissues) {
		int k;
		for (k=0; k<16; k++) {
			pat.setColorAt(0, getColor(CALC_CEILING_SHALLOW));
			pat.setColorAt(1, QColor(100, 100, 100, 50));

			entry = gc.pi.entry;
			p.clear();
			p.append(QPointF(SCALEGC(0, 0)));
			for (i = 0; i < gc.pi.nr; i++, entry++) {
				if ((entry->ceilings)[k])
					p.append(QPointF(SCALEGC(entry->sec, (entry->ceilings)[k])));
				else
					p.append(QPointF(SCALEGC(entry->sec, 0)));
			}
			p.append(QPointF(SCALEGC((entry-1)->sec, 0)));
			neatFill = new QGraphicsPolygonItem();
			neatFill->setPolygon(p);
			neatFill->setBrush(pat);
			scene()->addItem(neatFill);
		}
	}
	/* next show where we have been bad and crossed the dc's ceiling */
	if (prefs.profile_dc_ceiling) {
		pat.setColorAt(0, getColor(CEILING_SHALLOW));
		pat.setColorAt(1, getColor(CEILING_DEEP));

		entry = gc.pi.entry;
		p.clear();
		p.append(QPointF(SCALEGC(0, 0)));
		for (i = 0; i < gc.pi.nr; i++, entry++)
			p.append(QPointF(SCALEGC(entry->sec, entry->depth)));

		for (i-- , entry--; i >= 0; i--, entry--) {
			if (entry->in_deco && entry->stopdepth > entry->depth) {
				p.append(QPointF(SCALEGC(entry->sec, entry->stopdepth)));
			} else {
				p.append(QPointF(SCALEGC(entry->sec, entry->depth)));
			}
		}
	}
	neatFill = new QGraphicsPolygonItem();
	neatFill->setPolygon(p);
	neatFill->setPen(QPen(QBrush(Qt::NoBrush),0));
	neatFill->setBrush(QBrush(pat));
	scene()->addItem(neatFill);

	/* Now do it again for the velocity colors */
	entry = gc.pi.entry;
	for (i = 1; i < gc.pi.nr; i++) {
		entry++;
		sec = entry->sec;
		/* we want to draw the segments in different colors
		 * representing the vertical velocity, so we need to
		 * chop this into short segments */
		depth = entry->depth;
		QGraphicsLineItem *item = new QGraphicsLineItem(SCALEGC(entry[-1].sec, entry[-1].depth), SCALEGC(sec, depth));
		QPen pen(defaultPen);
		pen.setColor(getColor((color_indice_t)(VELOCITY_COLORS_START_IDX + entry->velocity)));
		item->setPen(pen);
		scene()->addItem(item);
	}
}

QGraphicsItemGroup *ProfileGraphicsView::plot_text(text_render_options_t *tro,const QPointF& pos, const QString& text, QGraphicsItem *parent)
{
	QFont fnt(font());
	QFontMetrics fm(fnt);

	if (printMode)
		fnt.setPixelSize(tro->size);

	QPointF point(SCALEGC(pos.x(), pos.y())); // This is neded because of the SCALE macro.
	double dx = tro->hpos * (fm.width(text));
	double dy = tro->vpos * (fm.height());

	QGraphicsItemGroup *group = new QGraphicsItemGroup(parent);
	QPainterPath textPath;
	/* addText() uses bottom-left text baseline and the -3 offset is probably slightly off
	 * for different font sizes. */
	textPath.addText(0, fm.height() - 3, fnt, text);
	QPainterPathStroker stroker;
	stroker.setWidth(3);
	QGraphicsPathItem *strokedItem = new QGraphicsPathItem(stroker.createStroke(textPath), group);
	strokedItem->setBrush(QBrush(getColor(TEXT_BACKGROUND)));
	strokedItem->setPen(Qt::NoPen);

	QGraphicsPathItem *textItem = new QGraphicsPathItem(textPath, group);
	textItem->setBrush(QBrush(getColor(tro->color)));
	textItem->setPen(Qt::NoPen);

	group->setPos(point.x() + dx, point.y() + dy);
	if (!printMode)
		group->setFlag(QGraphicsItem::ItemIgnoresTransformations);

	if (!parent)
		scene()->addItem(group);
	return group;
}

void ProfileGraphicsView::resizeEvent(QResizeEvent *event)
{
	refresh();
}

void ProfileGraphicsView::plot_temperature_profile()
{
	int last = 0;

	if (!setup_temperature_limits(&gc))
		return;

	QPointF from;
	QPointF to;
	QColor color = getColor(TEMP_PLOT);

	for (int i = 0; i < gc.pi.nr; i++) {
		struct plot_data *entry = gc.pi.entry + i;
		int mkelvin = entry->temperature;
		int sec = entry->sec;
		if (!mkelvin) {
			if (!last)
				continue;
			mkelvin = last;
		}
		if (last) {
			to = QPointF(SCALEGC(sec, mkelvin));
			QGraphicsLineItem *item = new QGraphicsLineItem(from.x(), from.y(), to.x(), to.y());
			QPen pen(defaultPen);
			pen.setColor(color);
			item->setPen(pen);
			scene()->addItem(item);
			from = to;
		} else {
			from = QPointF(SCALEGC(sec, mkelvin));
		}
		last = mkelvin;
	}
}

void ProfileGraphicsView::edit_dive_time(const QString& time)
{
	// this should set the full time of the dive.
	refresh();
}

void ProfileGraphicsView::on_rulerAction()
{
	rulerEnabled = !rulerEnabled;
	refresh();
}

void ProfileGraphicsView::on_scaleAction()
{
	zoomed_plot = !zoomed_plot;
	refresh();
}

void ToolTipItem::addToolTip(const QString& toolTip, const QIcon& icon)
{
	QGraphicsPixmapItem *iconItem = 0;
	double yValue = title->boundingRect().height() + SPACING;
	Q_FOREACH(ToolTip t, toolTips) {
		yValue += t.second->boundingRect().height();
	}
	if (!icon.isNull()) {
		iconItem = new QGraphicsPixmapItem(icon.pixmap(ICON_SMALL,ICON_SMALL), this);
		iconItem->setPos(SPACING, yValue);
	}

	QGraphicsSimpleTextItem *textItem = new QGraphicsSimpleTextItem(toolTip, this);
	textItem->setPos(SPACING + ICON_SMALL + SPACING, yValue);
	textItem->setBrush(QBrush(Qt::white));
	textItem->setFlag(ItemIgnoresTransformations);
	toolTips.push_back(qMakePair(iconItem, textItem));
	expand();
}

void ToolTipItem::refresh(struct graphics_context *gc, QPointF pos)
{
	clear();
	int time = (pos.x() * gc->maxtime) / gc->maxx;
	char buffer[500];
	get_plot_details(gc, time, buffer, 500);
	addToolTip(QString(buffer));

	QList<QGraphicsItem*> items = scene()->items(pos, Qt::IntersectsItemShape, Qt::DescendingOrder, transform());
	Q_FOREACH(QGraphicsItem *item, items) {
		if (!item->toolTip().isEmpty())
			addToolTip(item->toolTip());
	}

}

void ToolTipItem::clear()
{
	Q_FOREACH(ToolTip t, toolTips) {
		delete t.first;
		delete t.second;
	}
	toolTips.clear();
}

void ToolTipItem::setRect(const QRectF& r)
{
	// qDeleteAll(childItems());
	delete background;

	rectangle = r;
	setBrush(QBrush(Qt::white));
	setPen(QPen(Qt::black, 0.5));

	// Creates a 2pixels border
	QPainterPath border;
	border.addRoundedRect(-4, -4,  rectangle.width() + 8, rectangle.height() + 10, 3, 3);
	border.addRoundedRect(-1, -1,  rectangle.width() + 3, rectangle.height() + 4, 3, 3);
	setPath(border);

	QPainterPath bg;
	bg.addRoundedRect(-1, -1, rectangle.width() + 3, rectangle.height() + 4, 3, 3);

	QColor c = QColor(Qt::black);
	c.setAlpha(155);

	QGraphicsPathItem *b = new QGraphicsPathItem(bg, this);
	b->setFlag(ItemStacksBehindParent);
	b->setFlags(ItemIgnoresTransformations);
	b->setBrush(c);
	b->setPen(QPen(QBrush(Qt::transparent), 0));
	b->setZValue(-10);
	background = b;

	updateTitlePosition();
}

void ToolTipItem::collapse()
{
	QPropertyAnimation *animation = new QPropertyAnimation(this, "rect");
	animation->setDuration(100);
	animation->setStartValue(nextRectangle);
	animation->setEndValue(QRect(0, 0, ICON_SMALL, ICON_SMALL));
	animation->start(QAbstractAnimation::DeleteWhenStopped);
	clear();

	status = COLLAPSED;
}

void ToolTipItem::expand()
{
	if (!title)
		return;

	double width = 0, height = title->boundingRect().height() + SPACING;
	Q_FOREACH(ToolTip t, toolTips) {
		if (t.second->boundingRect().width() > width)
			width = t.second->boundingRect().width();
		height += t.second->boundingRect().height();
	}
	/*       Left padding, Icon Size,   space, right padding */
	width += SPACING       + ICON_SMALL + SPACING + SPACING;

	if (width < title->boundingRect().width() + SPACING*2)
		width = title->boundingRect().width() + SPACING*2;

	if (height < ICON_SMALL)
		height = ICON_SMALL;

	nextRectangle.setWidth(width);
	nextRectangle.setHeight(height);

	QPropertyAnimation *animation = new QPropertyAnimation(this, "rect");
	animation->setDuration(100);
	animation->setStartValue(rectangle);
	animation->setEndValue(nextRectangle);
	animation->start(QAbstractAnimation::DeleteWhenStopped);

	status = EXPANDED;
}

ToolTipItem::ToolTipItem(QGraphicsItem* parent): QGraphicsPathItem(parent), background(0)
{
	title = new QGraphicsSimpleTextItem(tr("Information"), this);
	separator = new QGraphicsLineItem(this);
	setFlags(ItemIgnoresTransformations | ItemIsMovable | ItemClipsChildrenToShape);
	status = COLLAPSED;
	updateTitlePosition();
	setZValue(99);
}

ToolTipItem::~ToolTipItem()
{
	clear();
}

void ToolTipItem::updateTitlePosition()
{
	if (rectangle.width() < title->boundingRect().width() + SPACING*4) {
		QRectF newRect = rectangle;
		newRect.setWidth(title->boundingRect().width() + SPACING*4);
		newRect.setHeight((newRect.height() && isExpanded()) ? newRect.height() : ICON_SMALL);
		setRect(newRect);
	}

	title->setPos(boundingRect().width()/2  - title->boundingRect().width()/2 -1, 0);
	title->setFlag(ItemIgnoresTransformations);
	title->setPen(QPen(Qt::white, 1));
	title->setBrush(Qt::white);

	if (toolTips.size() > 0) {
		double x1 = 3;
		double y1 = title->pos().y() + SPACING/2 + title->boundingRect().height();
		double x2 = boundingRect().width() - 10;
		double y2 = y1;

		separator->setLine(x1, y1, x2, y2);
		separator->setFlag(ItemIgnoresTransformations);
		separator->setPen(QPen(Qt::white));
		separator->show();
	} else {
		separator->hide();
	}
}

bool ToolTipItem::isExpanded() {
	return status == EXPANDED;
}

void ToolTipItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
	persistPos();
	QGraphicsPathItem::mouseReleaseEvent(event);
}

void ToolTipItem::persistPos()
{
	QPoint currentPos = scene()->views().at(0)->mapFromScene(pos());
	QSettings s;
	s.beginGroup("ProfileMap");
	s.setValue("tooltip_position", currentPos);
	s.endGroup();
}

void ToolTipItem::readPos()
{
	QSettings s;
	s.beginGroup("ProfileMap");
	QPointF value = scene()->views().at(0)->mapToScene(
		s.value("tooltip_position").toPoint()
	);
	if (!scene()->sceneRect().contains(value)) {
		value = QPointF(0,0);
	}
	setPos(value);
}

QColor EventItem::getColor(const color_indice_t i)
{
	return profile_color[i].at((isGrayscale) ? 1 : 0);
}

EventItem::EventItem(struct event *ev, QGraphicsItem* parent, bool grayscale): QGraphicsPixmapItem(parent), ev(ev), isGrayscale(grayscale)
{
	if (ev->name && (strcmp(ev->name, "bookmark") == 0 || strcmp(ev->name, "heading") == 0)) {
		setPixmap( QPixmap(QString(":flag")).scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	} else {
		setPixmap( QPixmap(QString(":warning")).scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}
}

RulerNodeItem::RulerNodeItem(QGraphicsItem *parent, graphics_context context) : QGraphicsEllipseItem(parent), gc(context), entry(NULL) , ruler(NULL)
{
	setRect(QRect(QPoint(-8,8),QPoint(8,-8)));
	setBrush(QColor(0xff, 0, 0, 127));
	setPen(QColor("#FF0000"));
	setFlag(QGraphicsItem::ItemIsMovable);
	setFlag(ItemSendsGeometryChanges);
	setFlag(ItemIgnoresTransformations);
}

void RulerNodeItem::setRuler(RulerItem *r)
{
	ruler = r;
}

void RulerNodeItem::recalculate()
{
	struct plot_info *pi = &gc.pi;
	struct plot_data *data = pi->entry+(pi->nr-1);
	uint16_t count = 0;
	if (x() < 0) {
		setPos(0, y());
	} else if (x() > SCALEXGC(data->sec)) {
		setPos(SCALEXGC(data->sec), y());
	} else {
		data = pi->entry;
		count=0;
		while (SCALEXGC(data->sec) < x() && count < pi->nr) {
			data = pi->entry+count;
			count++;
		}
		setPos(SCALEGC(data->sec, data->depth));
		entry=data;
	}
}

QVariant RulerNodeItem::itemChange(GraphicsItemChange change, const QVariant &value)
{
	if (change == ItemPositionHasChanged) {
		recalculate();
		if (ruler != NULL)
			ruler->recalculate();
		if (scene()) {
			scene()->update();
		}
	}
	return QGraphicsEllipseItem::itemChange(change, value);
}

RulerItem::RulerItem(QGraphicsItem *parent, RulerNodeItem *sourceNode, RulerNodeItem *destNode) : QGraphicsObject(parent), source(sourceNode), dest(destNode)
{
	recalculate();
}

void RulerItem::recalculate()
{
	char buffer[500];
	QPointF tmp;
	QFont font;
	QFontMetrics fm(font);

	if (source == NULL || dest == NULL)
		return;

	prepareGeometryChange();
	startPoint = mapFromItem(source, 0, 0);
	endPoint = mapFromItem(dest, 0, 0);
	if (startPoint.x() > endPoint.x()) {
		tmp = endPoint;
		endPoint = startPoint;
		startPoint = tmp;
	}
	QLineF line(startPoint, endPoint);

	compare_samples(source->entry, dest->entry, buffer, 500, 1);
	text = QString(buffer);

	QRect r = fm.boundingRect(QRect(QPoint(10,-1*INT_MAX), QPoint(line.length()-10, 0)), Qt::TextWordWrap, text);
	if (r.height() < 10)
		height = 10;
	else
		height = r.height();

	QLineF line_n = line.normalVector();
	line_n.setLength(height);
	if (scene()) {
		/* Determine whether we draw down or upwards */
		if (scene()->sceneRect().contains(line_n.p2()) &&
		    scene()->sceneRect().contains(endPoint+QPointF(line_n.dx(),line_n.dy())))
			paint_direction = -1;
		else
			paint_direction = 1;
	}
}

RulerNodeItem *RulerItem::sourceNode() const
{
	return source;
}

RulerNodeItem *RulerItem::destNode() const
{
	return dest;
}

void RulerItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
	QLineF line(startPoint, endPoint);
	QLineF line_n = line.normalVector();
	painter->setPen(QColor(Qt::black));
	painter->setBrush(Qt::NoBrush);
	line_n.setLength(height);

	if (paint_direction == 1)
		line_n.setAngle(line_n.angle()+180);
	painter->drawLine(line);
	painter->drawLine(line_n);
	painter->drawLine(line_n.p1() + QPointF(line.dx(), line.dy()), line_n.p2() + QPointF(line.dx(), line.dy()));

	//Draw Text
	painter->save();
	painter->translate(startPoint.x(), startPoint.y());
	painter->rotate(line.angle()*-1);
	if (paint_direction == 1)
		painter->translate(0, height);
	painter->setPen(Qt::black);
	painter->drawText(QRectF(QPointF(10,-1*height), QPointF(line.length()-10, 0)), Qt::TextWordWrap, text);
	painter->restore();
}

QRectF RulerItem::boundingRect() const
{
	return shape().controlPointRect();
}

QPainterPath RulerItem::shape() const
{
	QPainterPath path;
	QLineF line(startPoint, endPoint);
	QLineF line_n = line.normalVector();
	line_n.setLength(height);
	if (paint_direction == 1)
		line_n.setAngle(line_n.angle()+180);
	path.moveTo(startPoint);
	path.lineTo(line_n.p2());
	path.lineTo(line_n.p2() + QPointF(line.dx(), line.dy()));
	path.lineTo(endPoint);
	path.lineTo(startPoint);
	return path;
}

GraphicsTextEditor::GraphicsTextEditor(QGraphicsItem* parent): QGraphicsTextItem(parent)
{
}

void GraphicsTextEditor::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event)
{
	// Remove the proxy filter so we can focus here.
	mainWindow()->graphics()->setFocusProxy(0);
	setTextInteractionFlags(Qt::TextEditorInteraction | Qt::TextEditable);
}

void GraphicsTextEditor::keyReleaseEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return) {
		setTextInteractionFlags(Qt::NoTextInteraction);
		emit editingFinished( toPlainText() );
		mainWindow()->graphics()->setFocusProxy(mainWindow()->dive_list());
		return;
	}
	emit textChanged( toPlainText() );
	QGraphicsTextItem::keyReleaseEvent(event);
}
