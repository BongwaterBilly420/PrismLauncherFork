#include "BlockedModsDialog.h"
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QPushButton>
#include "Application.h"
#include "ui_BlockedModsDialog.h"


#include <QDebug>
#include <QStandardPaths>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>

BlockedModsDialog::BlockedModsDialog(QWidget* parent, const QString& title, const QString& text, QList<BlockedMod>& mods)
    : QDialog(parent), ui(new Ui::BlockedModsDialog), mods(mods)
{
    hashing_task = shared_qobject_ptr<ConcurrentTask>(new ConcurrentTask(this, "MakeHashesTask", 10));
    
    ui->setupUi(this);

    auto openAllButton = ui->buttonBox->addButton(tr("Open All"), QDialogButtonBox::ActionRole);
    connect(openAllButton, &QPushButton::clicked, this, &BlockedModsDialog::openAll);

    auto downloadFolderButton = ui->buttonBox->addButton(tr("Add Download Folder"), QDialogButtonBox::ActionRole);
    connect(downloadFolderButton, &QPushButton::clicked, this, &BlockedModsDialog::addDownloadFolder);

    connect(&watcher, &QFileSystemWatcher::directoryChanged, this, &BlockedModsDialog::directoryChanged);

    

    qDebug() << "Mods List: " << mods;

    setupWatch();
    scanPaths();

    this->setWindowTitle(title);
    ui->label->setText(text);
    ui->labelModsFound->setText(tr("Please download the missing mods."));

    setAcceptDrops(true);

    update();
}

BlockedModsDialog::~BlockedModsDialog()
{
    delete ui;
}

void BlockedModsDialog::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) {
        e->acceptProposedAction();
    }
}

void BlockedModsDialog::dropEvent(QDropEvent *e)
{
    foreach (const QUrl &url, e->mimeData()->urls()) {
        QString file = url.toLocalFile();
        qDebug() << "Dropped file:" << file;
        addHashTask(file);
    }
    hashing_task->start();
}

void BlockedModsDialog::openAll()
{
    for (auto& mod : mods) {
        QDesktopServices::openUrl(mod.websiteUrl);
    }
}

void BlockedModsDialog::addDownloadFolder() {
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("Select directory where you downloaded the mods"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        QFileDialog::ShowDirsOnly);
    watcher.addPath(dir);
    scanPath(dir);
}

/// @brief update UI with current status of the blocked mod detection
void BlockedModsDialog::update()
{
    QString text;
    QString span;

    for (auto& mod : mods) {
        if (mod.matched) {
            // &#x2714; -> html for HEAVY CHECK MARK : ✔
            span = QString(tr("<span style=\"color:green\"> &#x2714; Found at %1 </span>")).arg(mod.localPath);
        } else {
            // &#x2718; -> html for HEAVY BALLOT X : ✘
            span = QString(tr("<span style=\"color:red\"> &#x2718; Not Found </span>"));
        }
        text += QString(tr("%1: <a href='%2'>%2</a> <p>Hash: %3 %4</p> <br/>")).arg(mod.name, mod.websiteUrl, mod.hash, span);
    }

    ui->textBrowser->setText(text);

    if (allModsMatched()) {
        ui->labelModsFound->setText(tr("All mods found ✔"));
    } else {
        ui->labelModsFound->setText(tr("Please download the missing mods."));
    }
}

/// @brief Signal fired when a watched direcotry has changed
/// @param path the path to the changed directory
void BlockedModsDialog::directoryChanged(QString path)
{
    qDebug() << "Directory changed: " << path;
    validateMatchedMods();
    scanPath(path);
}

/// @brief add the user downloads folder and the global mods folder to the filesystem watcher
void BlockedModsDialog::setupWatch()
{
    const QString downloadsFolder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString modsFolder = APPLICATION->settings()->get("CentralModsDir").toString();
    watcher.addPath(downloadsFolder);
    watcher.addPath(modsFolder);
}

/// @brief scan all watched folder
void BlockedModsDialog::scanPaths()
{
    for (auto& dir : watcher.directories()) {
        scanPath(dir);
    }
}

/// @brief Scan the directory at path, skip paths that do not contain a file name
///        of a blocked mod we are looking for
/// @param path the directory to scan
void BlockedModsDialog::scanPath(QString path)
{
    QDir scan_dir(path);
    QDirIterator scan_it(path, QDir::Filter::Files | QDir::Filter::Hidden, QDirIterator::NoIteratorFlags);
    while (scan_it.hasNext()) {
        QString file = scan_it.next();

        if (!checkValidPath(file)) {
            continue;
        }

        addHashTask(file);
    }

    hashing_task->start();
}

/// @brief add a hashing task for the file located at path and connect it to check that hash against 
///        our blocked mods list
/// @param path the path to the local file being hashed
void BlockedModsDialog::addHashTask(QString path) {
    auto hash_task = Hashing::createBlockedModHasher(path, ModPlatform::Provider::FLAME, "sha1");

    qDebug() << "Creating Hash task for path: " << path;

    connect(hash_task.get(), &Task::succeeded, [this, hash_task, path] { checkMatchHash(hash_task->getResult(), path); });
    connect(hash_task.get(), &Task::failed, [path] { qDebug() << "Failed to hash path: " << path; });

    hashing_task->addTask(hash_task);
}

/// @brief check if the computed hash for the provided path matches a blocked
///        mod we are looking for
/// @param hash the computed hash for the provided path
/// @param path the path to the local file being compared
void BlockedModsDialog::checkMatchHash(QString hash, QString path)
{
    bool match = false;

    qDebug() << "Checking for match on hash: " << hash << "| From path:" << path;

    for (auto& mod : mods) {
        if (mod.matched) {
            continue;
        }
        if (mod.hash.compare(hash, Qt::CaseInsensitive) == 0) {
            mod.matched = true;
            mod.localPath = path;
            match = true;

            qDebug() << "Hash match found:" << mod.name << hash << "| From path:" << path;

            break;
        }
    }

    if (match) {
        update();
    }
}

/// @brief Check if the name of the file at path matches the name of a blocked mod we are searching for
/// @param path the path to check
/// @return boolean: did the path match the name of a blocked mod?
bool BlockedModsDialog::checkValidPath(QString path)
{
    QFileInfo file = QFileInfo(path);
    QString filename = file.fileName();

    for (auto& mod : mods) {
        if (mod.name.compare(filename, Qt::CaseInsensitive) == 0) {
            qDebug() << "Name match found:" << mod.name << "| From path:" << path;
            return true;
        }
    }

    return false;
}

bool BlockedModsDialog::allModsMatched()
{
    return std::all_of(mods.begin(), mods.end(), [](auto const& mod) { return mod.matched; });
}

/// @brief ensure matched file paths still exist
void BlockedModsDialog::validateMatchedMods() {
    bool changed = false;
    for (auto& mod : mods) {
        if (mod.matched) {
            QFileInfo file = QFileInfo(mod.localPath);
            if (!file.exists() || !file.isFile()) {
                mod.localPath = "";
                mod.matched = false;
                changed = true;
            }
        }
    }
    if (changed) {
        update();
    }
}

/// qDebug print support for the BlockedMod struct
QDebug operator<<(QDebug debug, const BlockedMod& m)
{
    QDebugStateSaver saver(debug);

    debug.nospace() << "{ name: " << m.name << ", websiteUrl: " << m.websiteUrl << ", hash: " << m.hash << ", matched: " << m.matched
                    << ", localPath: " << m.localPath << "}";

    return debug;
}
