/*
 * This file is part of the Nepomuk widgets collection
 * Copyright (c) 2013 Denis Steckelmacher <steckdenis@yahoo.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License version 2.1 as published by the Free Software Foundation,
 * or any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
*/

#include "groupedlineedit.h"

#include <QtGui/QStyleOptionFrameV3>
#include <QtGui/QFontMetrics>
#include <QtGui/QApplication>
#include <QtGui/QScrollBar>
#include <QtGui/QTextDocument>
#include <QtGui/QTextBlock>
#include <QtGui/QTextLayout>
#include <QtGui/QTextLine>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QPalette>

struct GroupedLineEdit::Private
{
    struct Block {
	    int start;
	    int end;
	    QString text;
    };
    QVector<Block> blocks;
    QVector<QColor> colors;
};

GroupedLineEdit::GroupedLineEdit(QWidget* parent)
: QPlainTextEdit(parent),
  d(new Private)
{
    setWordWrapMode(QTextOption::NoWrap);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    document()->setMaximumBlockCount(1);
}


GroupedLineEdit::~GroupedLineEdit()
{
    delete d;
}

QString GroupedLineEdit::text() const
{
    // Remove the block crosses from the text
    return toPlainText();
}

int GroupedLineEdit::cursorPosition() const
{
    return textCursor().positionInBlock();
}

void GroupedLineEdit::addBlock(int start, int end)
{
    Private::Block block;

    block.start = start;
    block.end = end;
    block.text = text().mid(start, end-start+1).trimmed();
    d->blocks.append(block);
    viewport()->update();
}

void GroupedLineEdit::addColor(QColor color)
{
    d->colors.append(color);
}

void GroupedLineEdit::removeAllColors()
{
	d->colors.clear();
}

QStringList GroupedLineEdit::getBlockStringList()
{
	QStringList retList;
	Private::Block block;
	foreach(block, d->blocks)
		retList.append(block.text);
	return retList;
}

void GroupedLineEdit::setCursorPosition(int position)
{
    QTextCursor c = textCursor();

    c.setPosition(position, QTextCursor::MoveAnchor);

    setTextCursor(c);
}

void GroupedLineEdit::setText(const QString &text)
{
    setPlainText(text);
}

void GroupedLineEdit::clear()
{
    QPlainTextEdit::clear();
    removeAllBlocks();
}

void GroupedLineEdit::selectAll()
{
    QTextCursor c = textCursor();

    c.select(QTextCursor::LineUnderCursor);

    setTextCursor(c);
}

void GroupedLineEdit::removeAllBlocks()
{
    d->blocks.clear();
    viewport()->update();
}

QSize GroupedLineEdit::sizeHint() const
{
    QSize rs(
        40,
        document()->findBlock(0).layout()->lineAt(0).height() +
            document()->documentMargin() * 2 +
            frameWidth() * 2
    );

    return rs;
}

QSize GroupedLineEdit::minimumSizeHint() const
{
    return sizeHint();
}

void GroupedLineEdit::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit editingFinished();
        return;
    }

    QPlainTextEdit::keyPressEvent(e);
}

void GroupedLineEdit::paintEvent(QPaintEvent *e)
{

    QTextLine line = document()->findBlock(0).layout()->lineAt(0);
    QPainter painter(viewport());

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::HighQualityAntialiasing, true);

    painter.fillRect(0, 0, viewport()->width(), viewport()->height(), palette().base());

    QVectorIterator<QColor> i(d->colors);
    i.toFront();
    foreach (const Private::Block &block, d->blocks) {
        qreal start_x = line.cursorToX(block.start, QTextLine::Trailing);
        qreal end_x = line.cursorToX(block.end + 1, QTextLine::Leading);
        QPainterPath path;
        QRectF rectangle(
            start_x - 1.0 - double(horizontalScrollBar()->value()),
            1.0,
            end_x - start_x + 2.0,
            double(viewport()->height() - 2)
        );
        if (! i.hasNext())
            i.toFront();
        path.addRoundedRect(rectangle, 5.0, 5.0);
        painter.setPen(i.peekNext());
        painter.setBrush(i.next().lighter(180));
        painter.drawPath(path);
    }

    QPlainTextEdit::paintEvent(e);
}