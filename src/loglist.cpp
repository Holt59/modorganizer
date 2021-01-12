/*
Copyright (C) 2012 Sebastian Herbord. All rights reserved.

This file is part of Mod Organizer.

Mod Organizer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Mod Organizer is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Mod Organizer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <QStyledItemDelegate>

#include "loglist.h"
#include "organizercore.h"

using namespace MOBase;

static LogModel* g_instance = nullptr;
const std::size_t MaxLines = 1000;

LogModel::LogModel()
{
}

void LogModel::create()
{
  g_instance = new LogModel;
}

LogModel& LogModel::instance()
{
  return *g_instance;
}

void LogModel::add(MOBase::log::Entry e)
{
  QMetaObject::invokeMethod(
    this, [this, e]{ onEntryAdded(std::move(e)); }, Qt::QueuedConnection);
}

void LogModel::clear()
{
  beginResetModel();
  m_entries.clear();
  endResetModel();
}

const std::deque<MOBase::log::Entry>& LogModel::entries() const
{
  return m_entries;
}

void LogModel::onEntryAdded(MOBase::log::Entry e)
{
  bool full = false;
  if (m_entries.size() > MaxLines) {
    m_entries.pop_front();
    full = true;
  }

  const int row = static_cast<int>(m_entries.size());

  if (!full) {
    beginInsertRows(QModelIndex(), row, row + 1);
  }

  m_entries.emplace_back(std::move(e));

  if (!full) {
    endInsertRows();
  } else {
    emit dataChanged(
      createIndex(row, 0),
      createIndex(row + 1, columnCount({})));
  }
}

QModelIndex LogModel::index(int row, int column, const QModelIndex&) const
{
  return createIndex(row, column, row);
}

QModelIndex LogModel::parent(const QModelIndex&) const
{
  return QModelIndex();
}

int LogModel::rowCount(const QModelIndex& parent) const
{
  if (parent.isValid())
    return 0;
  else
    return static_cast<int>(m_entries.size());
}

int LogModel::columnCount(const QModelIndex&) const
{
  return 3;
}

QVariant LogModel::data(const QModelIndex& index, int role) const
{
  using namespace std::chrono;

  const auto row = static_cast<std::size_t>(index.row());
  if (row >= m_entries.size()) {
    return {};
  }

  const auto& e = m_entries[row];

  if (role == Qt::DisplayRole) {
    if (index.column() == 0) {
      const auto ms = duration_cast<milliseconds>(e.time.time_since_epoch());
      const auto s = duration_cast<seconds>(ms);

      const std::time_t tt = s.count();
      const int frac = static_cast<int>(ms.count() % 1000);

      const auto time = QDateTime::fromTime_t(tt).time().addMSecs(frac);
      return time.toString("hh:mm:ss.zzz");
    } else if (index.column() == 2) {
      return QString::fromStdString(e.message);
    }
  }

  if (role == Qt::DecorationRole) {
    if (index.column() == 1) {
      switch (e.level) {
        case log::Warning:
          return QIcon(":/MO/gui/warning");

        case log::Error:
          return QIcon(":/MO/gui/problem");

        case log::Debug:
          return QIcon(":/MO/gui/debug");
        case log::Info:
          return QIcon(":/MO/gui/information");
        default:
          return {};
      }
    }
  }

  return QVariant();
}

QVariant LogModel::headerData(int, Qt::Orientation, int) const
{
  return {};
}

// this is mostly from https://stackoverflow.com/a/35418703/2666289
//
class WordItemDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  QString anchorAt(QString html, const QPoint& point) const
  {
    QTextDocument doc;
    doc.setMarkdown(html);

    auto textLayout = doc.documentLayout();
    Q_ASSERT(textLayout != 0);
    return textLayout->anchorAt(point);
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    auto options = option;
    initStyleOption(&options, index);

    painter->save();

    QTextDocument doc;
    doc.setMarkdown(options.text);

    options.text = "";
    options.widget->style()->drawControl(QStyle::CE_ItemViewItem, &option, painter);

    painter->translate(options.rect.left(), options.rect.top());
    QRect clip(0, 0, options.rect.width(), options.rect.height());
    doc.drawContents(painter, clip);

    painter->restore();
  }

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    QStyleOptionViewItemV4 options = option;
    initStyleOption(&options, index);

    QTextDocument doc;
    doc.setMarkdown(options.text);
    doc.setTextWidth(options.rect.width());
    return QSize(doc.idealWidth(), doc.size().height());
  }
};

LogList::LogList(QWidget* parent)
  : QTreeView(parent), m_core(nullptr)
{
  setModel(&LogModel::instance());

  header()->setMinimumSectionSize(0);
  header()->resizeSection(1, 20);
  header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

  setAutoScroll(true);
  scrollToBottom();

  setMouseTracking(true);
  setItemDelegateForColumn(2, new WordItemDelegate(this));

  connect(
    this, &QWidget::customContextMenuRequested,
    [&](auto&& pos){ onContextMenu(pos); });

  connect(model(), &LogModel::rowsInserted, this, [&]{ onNewEntry(); });
  connect(model(), &LogModel::dataChanged, this, [&]{ onNewEntry(); });

  m_timer.setSingleShot(true);
  connect(&m_timer, &QTimer::timeout, [&]{ scrollToBottom(); });
}


void LogList::mousePressEvent(QMouseEvent* event) {
  QTreeView::mousePressEvent(event);

  auto anchor = anchorAt(event->pos());
  m_mousePressAnchor = anchor;
}

void LogList::mouseMoveEvent(QMouseEvent* event) {
  auto anchor = anchorAt(event->pos());

  if (m_mousePressAnchor != anchor) {
    m_mousePressAnchor.clear();
  }

  if (m_lastHoveredAnchor != anchor) {
    m_lastHoveredAnchor = anchor;
    if (!m_lastHoveredAnchor.isEmpty()) {
      QApplication::setOverrideCursor(QCursor(Qt::PointingHandCursor));
    }
    else {
      QApplication::restoreOverrideCursor();
    }
  }
}

void LogList::mouseReleaseEvent(QMouseEvent* event) {
  if (!m_mousePressAnchor.isEmpty()) {
    auto anchor = anchorAt(event->pos());

    if (anchor == m_mousePressAnchor) {
      emit linkActivated(m_mousePressAnchor);
    }

    m_mousePressAnchor.clear();
  }

  QTreeView::mouseReleaseEvent(event);
}

QString LogList::anchorAt(const QPoint& pos) const {
  auto index = indexAt(pos);
  if (index.isValid()) {
    auto delegate = itemDelegate(index);
    auto wordDelegate = dynamic_cast<WordItemDelegate*>(delegate);
    if (wordDelegate != 0) {
      auto itemRect = visualRect(index);
      auto relativeClickPosition = pos - itemRect.topLeft();

      auto html = model()->data(index, Qt::DisplayRole).toString();

      return wordDelegate->anchorAt(html, relativeClickPosition);
    }
  }

  return QString();
}

void LogList::onNewEntry()
{
  m_timer.start(std::chrono::milliseconds(10));
}

void LogList::setCore(OrganizerCore& core)
{
  m_core = &core;
}

void LogList::copyToClipboard()
{
  std::string s;

  auto* m = static_cast<LogModel*>(model());
  for (const auto& e : m->entries()) {
    s += e.formattedMessage + "\n";
  }

  if (!s.empty()) {
    // last newline
    s.pop_back();
  }

  QApplication::clipboard()->setText(QString::fromStdString(s));
}

void LogList::clear()
{
  static_cast<LogModel*>(model())->clear();
}

void LogList::openLogsFolder()
{
  QString logsPath = qApp->property("dataPath").toString() + "/" + QString::fromStdWString(AppConfig::logPath());
  shell::Explore(logsPath);
}

QMenu* LogList::createMenu(QWidget* parent)
{
  auto* menu = new QMenu(parent);

  menu->addAction(tr("&Copy all"), [&]{ copyToClipboard(); });
  menu->addSeparator();
  menu->addAction(tr("C&lear all"), [&]{ clear(); });
  menu->addAction(tr("&Open logs folder"), [&]{ openLogsFolder(); });

  auto* levels = new QMenu(tr("&Level"));
  menu->addMenu(levels);

  auto* ag = new QActionGroup(menu);

  auto addAction = [&](auto&& text, auto&& level) {
    auto* a = new QAction(text, ag);

    a->setCheckable(true);
    a->setChecked(log::getDefault().level() == level);

    connect(a, &QAction::triggered, [this, level]{
      if (m_core) {
        m_core->setLogLevel(level);
      }
    });

    levels->addAction(a);
  };

  addAction(tr("&Debug"), log::Debug);
  addAction(tr("&Info"), log::Info);
  addAction(tr("&Warnings"), log::Warning);
  addAction(tr("&Errors"), log::Error);

  return menu;
}

void LogList::onContextMenu(const QPoint& pos)
{
  auto* menu = createMenu(this);
  menu->popup(viewport()->mapToGlobal(pos));
}


log::Levels convertQtLevel(QtMsgType t)
{
  switch (t)
  {
    case QtDebugMsg:
      return log::Debug;

    case QtWarningMsg:
      return log::Warning;

    case QtCriticalMsg:  // fall-through
    case QtFatalMsg:
      return log::Error;

    case QtInfoMsg:  // fall-through
    default:
      return log::Info;
  }
}

void qtLogCallback(
  QtMsgType type, const QMessageLogContext& context, const QString& message)
{
  std::string_view file = "";

  if (type != QtDebugMsg) {
    if (context.file) {
      file = context.file;

      const auto lastSep = file.find_last_of("/\\");
      if (lastSep != std::string_view::npos) {
        file = {context.file + lastSep + 1};
      }
    }
  }

  if (file.empty()) {
    log::log(
      convertQtLevel(type), "{}",
      message.toStdString());
  } else {
    log::log(
      convertQtLevel(type), "[{}:{}] {}",
      file, context.line, message.toStdString());
  }
}

void initLogging()
{
  LogModel::create();

  log::LoggerConfiguration conf;
  conf.maxLevel = MOBase::log::Debug;
  conf.pattern = "%^[%Y-%m-%d %H:%M:%S.%e %L] %v%$";
  conf.utc = true;

  log::createDefault(conf);

  log::getDefault().setCallback(
    [](log::Entry e){ LogModel::instance().add(e); });

  qInstallMessageHandler(qtLogCallback);
}

bool createAndMakeWritable(const std::wstring &subPath) {
  QString const dataPath = qApp->property("dataPath").toString();
  QString fullPath = dataPath + "/" + QString::fromStdWString(subPath);

  if (!QDir(fullPath).exists() && !QDir().mkdir(fullPath)) {
    QMessageBox::critical(nullptr, QObject::tr("Error"),
      QObject::tr("Failed to create \"%1\". Your user "
        "account probably lacks permission.")
      .arg(fullPath));
    return false;
  } else {
    return true;
  }
}

bool setLogDirectory(const QString& dir)
{
  const auto logFile =
    dir + "/" +
    QString::fromStdWString(AppConfig::logPath()) + "/" +
    QString::fromStdWString(AppConfig::logFileName());

  if (!createAndMakeWritable(AppConfig::logPath())) {
    return false;
  }

  log::getDefault().setFile(MOBase::log::File::single(logFile.toStdWString()));

  return true;
}
