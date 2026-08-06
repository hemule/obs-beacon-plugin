/*
Beacon
Copyright (C) 2026 hemule <hemule@mayflower.work>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "log-window.hpp"
#include "log-capture.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>

namespace beacon {

namespace {
constexpr int kRefreshMs = 1000;
}

LogWindow::LogWindow(LogCapture *logCapture, QWidget *parent) : QWidget(parent, Qt::Window), logCapture_(logCapture)
{
	setObjectName("BeaconLogWindow");
	setWindowTitle(QString::fromUtf8(obs_module_text("LogWindowTitle")));
	resize(760, 420);
	buildUi();

	timer_ = new QTimer(this);
	timer_->setInterval(kRefreshMs);
	connect(timer_, &QTimer::timeout, this, &LogWindow::refresh);
	// Started/stopped in showEvent/hideEvent so a hidden window costs nothing.
}

LogWindow::~LogWindow() = default;

void LogWindow::detach()
{
	if (timer_)
		timer_->stop();
	logCapture_ = nullptr;
}

void LogWindow::buildUi()
{
	auto *root = new QVBoxLayout(this);

	auto *controls = new QHBoxLayout();
	filterCheck_ = new QCheckBox(QString::fromUtf8(obs_module_text("OnlyConnection")));
	filterCheck_->setChecked(onlyConnection_);
	connect(filterCheck_, &QCheckBox::toggled, this, &LogWindow::onFilterToggled);
	controls->addWidget(filterCheck_);
	controls->addStretch(1);

	auto *copyBtn = new QPushButton(QString::fromUtf8(obs_module_text("Copy")));
	connect(copyBtn, &QPushButton::clicked, this, &LogWindow::onCopy);
	controls->addWidget(copyBtn);

	auto *clearBtn = new QPushButton(QString::fromUtf8(obs_module_text("Clear")));
	connect(clearBtn, &QPushButton::clicked, this, &LogWindow::onClear);
	controls->addWidget(clearBtn);

	root->addLayout(controls);

	logView_ = new QPlainTextEdit();
	logView_->setReadOnly(true);
	logView_->setMaximumBlockCount(static_cast<int>(LogCapture::kMaxEntries) + 16);
	logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	logView_->setLineWrapMode(QPlainTextEdit::NoWrap);
	root->addWidget(logView_, 1);
}

void LogWindow::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	refreshLogs(true);
	if (timer_)
		timer_->start();
}

void LogWindow::hideEvent(QHideEvent *event)
{
	if (timer_)
		timer_->stop();
	QWidget::hideEvent(event);
}

void LogWindow::refresh()
{
	refreshLogs(false);
}

void LogWindow::refreshLogs(bool force)
{
	if (!logCapture_)
		return;

	const uint64_t gen = logCapture_->generation();
	if (!force && gen == lastLogGeneration_)
		return;
	lastLogGeneration_ = gen;

	auto *bar = logView_->verticalScrollBar();
	const bool atBottom = bar->value() >= bar->maximum() - 2;

	const auto entries = logCapture_->snapshot(onlyConnection_);
	QString text;
	text.reserve(static_cast<int>(entries.size()) * 64);
	for (const auto &e : entries) {
		text += QString::fromStdString(e.timestamp);
		text += QLatin1String("  ");
		text += QString::fromStdString(e.text);
		text += QLatin1Char('\n');
	}
	logView_->setPlainText(text);

	if (atBottom)
		bar->setValue(bar->maximum());
}

void LogWindow::onFilterToggled(bool onlyConnection)
{
	onlyConnection_ = onlyConnection;
	refreshLogs(true);
}

void LogWindow::onCopy()
{
	if (auto *cb = QGuiApplication::clipboard())
		cb->setText(logView_->toPlainText());
}

void LogWindow::onClear()
{
	if (logCapture_)
		logCapture_->clear();
	logView_->clear();
	lastLogGeneration_ = logCapture_ ? logCapture_->generation() : 0;
}

} // namespace beacon
