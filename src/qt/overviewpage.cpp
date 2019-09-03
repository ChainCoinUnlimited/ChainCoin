// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2011-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <init.h>
#include <interfaces/node.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/utilitydialog.h>
#include <qt/walletmodel.h>

#include <qt/coinjoinconfig.h>

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QSettings>
#include <QTimer>

#define ICON_OFFSET 16
#define DECORATION_SIZE 54
#define NUM_ITEMS 5
#define NUM_ITEMS_ADV 7

Q_DECLARE_METATYPE(interfaces::WalletBalances)

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    TxViewDelegate(const PlatformStyle *_platformStyle, QObject *parent=nullptr):
        QAbstractItemDelegate(parent), unit(BitcoinUnits::CHC),
        platformStyle(_platformStyle)
    {

    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect;
        mainRect.moveLeft(ICON_OFFSET);
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace - ICON_OFFSET, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon = platformStyle->SingleColorIcon(icon);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft|Qt::AlignVCenter, address, &boundingRect);

        if (index.data(TransactionTableModel::WatchonlyRole).toBool())
        {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            QRect watchonlyRect(boundingRect.right() + 5, mainRect.top()+ypad+halfheight, 16, halfheight);
            iconWatchonly.paint(painter, watchonlyRect);
        }

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = BitcoinUnits::floorWithUnit(unit, amount, true, BitcoinUnits::separatorAlways);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }
        painter->drawText(amountRect, Qt::AlignRight|Qt::AlignVCenter, amountText);

        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(amountRect, Qt::AlignLeft|Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return QSize(DECORATION_SIZE, DECORATION_SIZE);
    }

    int unit;
    const PlatformStyle *platformStyle;

};
#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    clientModel(nullptr),
    walletModel(nullptr),
    txdelegate(new TxViewDelegate(platformStyle, this))

{
    ui->setupUi(this);

    m_balances.balance = -1;

    // use a SingleColorIcon for the "out of sync warning" icon
    QIcon icon = platformStyle->SingleColorIcon(":/icons/warning");
    icon.addPixmap(icon.pixmap(QSize(64,64), QIcon::Normal), QIcon::Disabled); // also set the disabled icon because we are using a disabled QPushButton to work around missing HiDPI support of QLabel (https://bugreports.qt.io/browse/QTBUG-42503)
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus->setIcon(icon);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    // Note: minimum height of listTransactions will be set later in updateAdvancedPSUI() to reflect actual settings
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, &QListView::clicked, this, &OverviewPage::handleTransactionClicked);

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
    connect(ui->labelWalletStatus, &QPushButton::clicked, this, &OverviewPage::handleOutOfSyncWarningClicks);
    connect(ui->labelTransactionsStatus, &QPushButton::clicked, this, &OverviewPage::handleOutOfSyncWarningClicks);
    ui->labelCoinJoinSyncStatus->setText("(" + tr("out of sync") + ")");

    // hide PS frame (helps to preserve saved size)
    // we'll setup and make it visible in updateAdvancedPSUI() later if we are not in litemode
    ui->frameCoinJoin->setVisible(false);

    // that's it for litemode
    if(fLiteMode) return;

    // Disable any PS UI when autobackup is disabled or failed for whatever reason
    if(nWalletBackups <= 0){
        DisableCoinJoinCompletely();
        ui->labelCoinJoinEnabled->setToolTip(tr("Automatic backups are disabled, no mixing available!"));
    } else {
        if(!m_coinjoinstatus.enabled){
            ui->toggleCoinJoin->setText(tr("Start Mixing"));
        } else {
            ui->toggleCoinJoin->setText(tr("Stop Mixing"));
        }
    }
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::handleOutOfSyncWarningClicks()
{
    Q_EMIT outOfSyncWarningClicked();
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    m_balances = balances;
    if (walletModel->privateKeysDisabled()) {
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, balances.watch_only_balance, false, BitcoinUnits::separatorAlways));
        ui->labelUnconfirmed->setText(BitcoinUnits::formatWithUnit(unit, balances.unconfirmed_watch_only_balance, false, BitcoinUnits::separatorAlways));
        ui->labelImmature->setText(BitcoinUnits::formatWithUnit(unit, balances.immature_watch_only_balance, false, BitcoinUnits::separatorAlways));
        ui->labelTotal->setText(BitcoinUnits::formatWithUnit(unit, balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance, false, BitcoinUnits::separatorAlways));
    } else {
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, balances.balance, false, BitcoinUnits::separatorAlways));
        ui->labelUnconfirmed->setText(BitcoinUnits::formatWithUnit(unit, balances.unconfirmed_balance, false, BitcoinUnits::separatorAlways));
        ui->labelImmature->setText(BitcoinUnits::formatWithUnit(unit, balances.immature_balance, false, BitcoinUnits::separatorAlways));
        ui->labelAnonymized->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.anonymized_balance, false, BitcoinUnits::separatorAlways));
        ui->labelTotal->setText(BitcoinUnits::formatWithUnit(unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance, false, BitcoinUnits::separatorAlways));
        ui->labelWatchAvailable->setText(BitcoinUnits::formatWithUnit(unit, balances.watch_only_balance, false, BitcoinUnits::separatorAlways));
        ui->labelWatchPending->setText(BitcoinUnits::formatWithUnit(unit, balances.unconfirmed_watch_only_balance, false, BitcoinUnits::separatorAlways));
        ui->labelWatchImmature->setText(BitcoinUnits::formatWithUnit(unit, balances.immature_watch_only_balance, false, BitcoinUnits::separatorAlways));
        ui->labelWatchTotal->setText(BitcoinUnits::formatWithUnit(unit, balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance, false, BitcoinUnits::separatorAlways));
    }
    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = balances.immature_balance != 0;
    bool showWatchOnlyImmature = balances.immature_watch_only_balance != 0;

    // for symmetry reasons also show immature label when the watch-only one is shown
    ui->labelImmature->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelImmatureText->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelWatchImmature->setVisible(!walletModel->privateKeysDisabled() && showWatchOnlyImmature); // show watch-only immature balance
}

// show/hide watch-only labels
void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal->setVisible(showWatchOnly);     // show watch-only total balance

    if (!showWatchOnly){
        ui->labelWatchImmature->hide();
    }
    else{
        ui->labelBalance->setIndent(20);
        ui->labelUnconfirmed->setIndent(20);
        ui->labelImmature->setIndent(20);
        ui->labelTotal->setIndent(20);
    }
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if (model) {
        // Show warning, for example if this is a prerelease version
        connect(model, &ClientModel::alertsChanged, this, &OverviewPage::updateAlerts);
        updateAlerts(model->getStatusBarWarnings());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // update the display unit, to not use the default ("CHC")
        updateDisplayUnit();
        // Keep up to date with wallet
        interfaces::Wallet& wallet = model->wallet();
        interfaces::WalletBalances balances = wallet.getBalances();
        setBalance(balances);
        connect(model, &WalletModel::balanceChanged, this, &OverviewPage::setBalance);

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &OverviewPage::updateDisplayUnit);

        updateWatchOnlyLabels(wallet.haveWatchOnly() && !model->privateKeysDisabled());
        connect(model, &WalletModel::notifyWatchonlyChanged, [this](bool showWatchOnly) {
            updateWatchOnlyLabels(showWatchOnly && !walletModel->privateKeysDisabled());
        });

        // explicitly update PS frame and transaction list to reflect actual settings
        updateAdvancedPSUI(model->getOptionsModel()->getShowAdvancedPSUI());

        // that's it for litemode
        if(fLiteMode) return;
        interfaces::CoinJoinStatus status = wallet.getCoinJoinStatus();
        coinJoinStatus(status);
        connect(model, &WalletModel::coinJoinChanged, this, &OverviewPage::coinJoinStatus);

        connect(model->getOptionsModel(), &OptionsModel::advancedPSUIChanged, this, &OverviewPage::updateAdvancedPSUI);

        connect(ui->coinJoinReset, &QPushButton::clicked, this, &OverviewPage::coinJoinReset);
        connect(ui->coinJoinInfo, &QPushButton::clicked, this, &OverviewPage::coinJoinInfo);
        connect(ui->toggleCoinJoin, &QPushButton::clicked, this, &OverviewPage::toggleCoinJoin);

        // coinjoin buttons will not react to spacebar must be clicked on
        ui->coinJoinReset->setFocusPolicy(Qt::NoFocus);
        ui->coinJoinInfo->setFocusPolicy(Qt::NoFocus);
        ui->toggleCoinJoin->setFocusPolicy(Qt::NoFocus);

        // Disable coinjoinClient builtin support for automatic backups while we are in GUI,
        // we'll handle automatic backups and user warnings in coinJoinStatus()
        walletModel->disableAutoBackup();
    }
}

void OverviewPage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
        if (m_balances.balance != -1) {
            setBalance(m_balances);
        }

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelCoinJoinSyncStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::updateCoinJoinProgress()
{
    if (!clientModel) return;

    QString strAmountAndRounds;
    QString strCoinJoinAmount = BitcoinUnits::formatHtmlWithUnit(nDisplayUnit, m_coinjoinstatus.amount * COIN, false, BitcoinUnits::separatorAlways);

    if(m_balances.balance == 0) {
        ui->coinJoinProgress->setValue(0);
        ui->coinJoinProgress->setToolTip(tr("No inputs detected"));

        // when balance is zero just show info from settings
        strCoinJoinAmount = strCoinJoinAmount.remove(strCoinJoinAmount.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = strCoinJoinAmount + " / " + tr("Depth: %n", nullptr, m_coinjoinstatus.depth);

        ui->labelAmountRounds->setToolTip(tr("No inputs detected"));
        ui->labelAmountRounds->setText(strAmountAndRounds);
        return;
    }

    CAmount nMaxToAnonymize = m_balances.balance;

    // If it's more than the anon threshold, limit to that.
    if(nMaxToAnonymize > m_coinjoinstatus.amount * COIN) nMaxToAnonymize = m_coinjoinstatus.amount * COIN;

    if(nMaxToAnonymize == 0) return;

    if(nMaxToAnonymize >= m_coinjoinstatus.amount * COIN) {
        ui->labelAmountRounds->setToolTip(tr("Found enough compatible inputs to anonymize %1")
                                          .arg(strCoinJoinAmount));
        strCoinJoinAmount = strCoinJoinAmount.remove(strCoinJoinAmount.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = strCoinJoinAmount + " / " + tr("Depth: %n", nullptr, m_coinjoinstatus.depth);
    } else {
        QString strMaxToAnonymize = BitcoinUnits::formatHtmlWithUnit(nDisplayUnit, nMaxToAnonymize, false, BitcoinUnits::separatorAlways);
        ui->labelAmountRounds->setToolTip(tr("Not enough compatible inputs to anonymize <span style='color:red;'>%1</span>,<br>"
                                             "will anonymize <span style='color:red;'>%2</span> instead")
                                          .arg(strCoinJoinAmount)
                                          .arg(strMaxToAnonymize));
        strMaxToAnonymize = strMaxToAnonymize.remove(strMaxToAnonymize.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = "<span style='color:red;'>" +
                QString(BitcoinUnits::factor(nDisplayUnit) == 1 ? "" : "~") + strMaxToAnonymize +
                " / " + tr("Depth: %n", nullptr, m_coinjoinstatus.depth) + "</span>";
    }
    ui->labelAmountRounds->setText(strAmountAndRounds);

    if (!fShowAdvancedPSUI) return;

    float progress = m_balances.mixing_progress;

    ui->coinJoinProgress->setValue(progress);

    QString strToolPip = ("<b>" + tr("Overall progress") + ": %1%</b><br/>")
            .arg(progress);
    ui->coinJoinProgress->setToolTip(strToolPip);
}

void OverviewPage::updateAdvancedPSUI(bool _fShowAdvancedPSUI) {
    this->fShowAdvancedPSUI = _fShowAdvancedPSUI;
    int nNumItems = (fLiteMode || !_fShowAdvancedPSUI) ? NUM_ITEMS : NUM_ITEMS_ADV;
    SetupTransactionList(nNumItems);

    if (fLiteMode) return;

    ui->frameCoinJoin->setVisible(true);
    ui->labelCompletitionText->setVisible(_fShowAdvancedPSUI);
    ui->coinJoinProgress->setVisible(_fShowAdvancedPSUI);
    ui->labelSubmittedDenomText->setVisible(_fShowAdvancedPSUI);
    ui->labelSubmittedDenom->setVisible(_fShowAdvancedPSUI);
    ui->coinJoinReset->setVisible(_fShowAdvancedPSUI);
    ui->coinJoinInfo->setVisible(true);
    ui->labelCoinJoinLastMessage->setVisible(_fShowAdvancedPSUI);
}

void OverviewPage::coinJoinStatus(const interfaces::CoinJoinStatus& status)
{
    if (!clientModel) return;
    if (!walletModel) return;

    m_coinjoinstatus = status;

    static int64_t nLastDSProgressBlockTime = 0;

    int nBestHeight = clientModel->cachedBestHeaderHeight;

    // We are processing more then 1 block per second, we'll just leave
    if(((nBestHeight - m_coinjoinstatus.cachednumblocks) / (GetTimeMillis() - nLastDSProgressBlockTime + 1) > 1)) return;
    nLastDSProgressBlockTime = GetTimeMillis();

    QString strKeysLeftText(tr("keys left: %1").arg(m_coinjoinstatus.keysleft));
    if(m_coinjoinstatus.keysleft < walletModel->m_privsendconfig.keyswarning) {
        strKeysLeftText = "<span style='color:red;'>" + strKeysLeftText + "</span>";
    }
    ui->labelCoinJoinEnabled->setToolTip(strKeysLeftText);

    if (!m_coinjoinstatus.enabled) {
        if (nBestHeight != m_coinjoinstatus.cachednumblocks) {
            walletModel->setNumBlocks(nBestHeight);
        }
        updateCoinJoinProgress();

        ui->labelCoinJoinLastMessage->setText("");
        ui->toggleCoinJoin->setText(tr("Start Mixing"));

        QString strEnabled = tr("Enabled / Not active");
        // Show how many keys left in advanced PS UI mode only
        if (fShowAdvancedPSUI) strEnabled += ", " + strKeysLeftText;
        ui->labelCoinJoinEnabled->setText(strEnabled);

        return;
    } else {
        ui->toggleCoinJoin->setText(tr("Stop Mixing"));
    }

    // Warn user that wallet is running out of keys
    // NOTE: we do NOT warn user and do NOT create autobackups if mixing is not running
    if (nWalletBackups > 0 && m_coinjoinstatus.keysleft < walletModel->m_privsendconfig.keyswarning) {
        QSettings settings;
        if(settings.value("fLowKeysWarning").toBool()) {
            QString strWarn =   tr("Very low number of keys left since last automatic backup!") + "<br><br>" +
                                tr("We are about to create a new automatic backup for you, however "
                                   "<span style='color:red;'> you should always make sure you have backups "
                                   "saved in some safe place</span>!") + "<br><br>" +
                                tr("Note: You can turn this message off in options.");
            ui->labelCoinJoinEnabled->setToolTip(strWarn);
            LogPrintf("OverviewPage::coinJoinStatus -- Very low number of keys left since last automatic backup, warning user and trying to create new backup...\n");
            QMessageBox::warning(this, tr("CoinJoin"), strWarn, QMessageBox::Ok, QMessageBox::Ok);
        } else {
            LogPrintf("OverviewPage::coinJoinStatus -- Very low number of keys left since last automatic backup, skipping warning and trying to create new backup...\n");
        }

        std::string strBackupWarning;
        std::string strBackupError;
        const std::string name = walletModel->getWalletName().toStdString();
        if(!walletModel->wallet().DoAutoBackup(name, strBackupWarning, strBackupError)) {
            if (!strBackupWarning.empty()) {
                // It's still more or less safe to continue but warn user anyway
                LogPrintf("OverviewPage::coinJoinStatus -- WARNING! Something went wrong on automatic backup: %s\n", strBackupWarning);

                QMessageBox::warning(this, tr("CoinJoin"),
                    tr("WARNING! Something went wrong on automatic backup") + ":<br><br>" + strBackupWarning.c_str(),
                    QMessageBox::Ok, QMessageBox::Ok);
            }
            if (!strBackupError.empty()) {
                // Things are really broken, warn user and stop mixing immediately
                LogPrintf("OverviewPage::coinJoinStatus -- ERROR! Failed to create automatic backup: %s\n", strBackupError);

                QMessageBox::warning(this, tr("CoinJoin"),
                    tr("ERROR! Failed to create automatic backup") + ":<br><br>" + strBackupError.c_str() + "<br>" +
                    tr("Mixing is disabled, please close your wallet and fix the issue!"),
                    QMessageBox::Ok, QMessageBox::Ok);
            }
        }
    }

    QString strEnabled = m_coinjoinstatus.enabled ? tr("Enabled") : tr("Disabled");
    // Show how many keys left in advanced PS UI mode only
    if(fShowAdvancedPSUI) strEnabled += ", " + strKeysLeftText;
    ui->labelCoinJoinEnabled->setText(strEnabled);

    if(nWalletBackups == -1) {
        // Automatic backup failed, nothing else we can do until user fixes the issue manually
        DisableCoinJoinCompletely();

        QString strError =  tr("ERROR! Failed to create automatic backup") + ", " +
                            tr("see debug.log for details.") + "<br><br>" +
                            tr("Mixing is disabled, please close your wallet and fix the issue!");
        ui->labelCoinJoinEnabled->setToolTip(strError);

        return;
    } else if(nWalletBackups == -2) {
        // We were able to create automatic backup but keypool was not replenished because wallet is locked.
        QString strWarning = tr("WARNING! Failed to replenish keypool, please unlock your wallet to do so.");
        ui->labelCoinJoinEnabled->setToolTip(strWarning);
    }

    // check darksend status and unlock if needed
    if(nBestHeight != m_coinjoinstatus.cachednumblocks) {
        // Balance and number of transactions might have changed
        walletModel->setNumBlocks(nBestHeight);
        updateCoinJoinProgress();
    }

    QString strStatus = QString::fromStdString(m_coinjoinstatus.status);

    QString s = tr("CoinJoin status:\n") + strStatus;

    if(s != ui->labelCoinJoinLastMessage->text())
        LogPrintf("OverviewPage::coinJoinStatus -- CoinJoin status: %s\n", strStatus.toStdString());

    ui->labelCoinJoinLastMessage->setText(s);

    ui->labelSubmittedDenom->setText(QString::fromStdString(m_coinjoinstatus.denom));

}

void OverviewPage::coinJoinReset(){
    walletModel->toggleMixing(true);
    walletModel->resetPool();
    walletModel->updateTransaction();

    QMessageBox::warning(this, tr("CoinJoin"),
        tr("CoinJoin was successfully reset."),
        QMessageBox::Ok, QMessageBox::Ok);
}

void OverviewPage::coinJoinInfo(){
    if (!clientModel) return;
    HelpMessageDialog dlg(clientModel->node(), this, HelpMessageDialog::pshelp);
    dlg.exec();
}

void OverviewPage::toggleCoinJoin(){
    QSettings settings;
    // Popup some information on first mixing
    QString hasMixed = settings.value("hasMixed").toString();
    if(hasMixed.isEmpty()){
        QMessageBox::information(this, tr("CoinJoin"),
                tr("If you don't want to see internal CoinJoin fees/transactions select \"Most Common\" as Type on the \"Transactions\" tab."),
                QMessageBox::Ok, QMessageBox::Ok);
        settings.setValue("hasMixed", "hasMixed");
    }
    if(!m_coinjoinstatus.enabled){
        const CAmount nMinAmount = walletModel->m_privsendconfig.minamount;
        if(m_balances.balance < nMinAmount){
            QString strMinAmount(BitcoinUnits::formatWithUnit(nDisplayUnit, nMinAmount));
            QMessageBox::warning(this, tr("CoinJoin"),
                tr("CoinJoin requires at least %1 to use.").arg(strMinAmount),
                QMessageBox::Ok, QMessageBox::Ok);
            return;
        }

        // if wallet is locked, ask for a passphrase
        if (walletModel->getEncryptionStatus() == WalletModel::Locked)
        {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock(true));
            if(!ctx.isValid())
            {
                //unlock was cancelled
                walletModel->setNumBlocks(std::numeric_limits<int>::max());
                QMessageBox::warning(this, tr("CoinJoin"),
                    tr("Wallet is locked and user declined to unlock. Disabling CoinJoin."),
                    QMessageBox::Ok, QMessageBox::Ok);
                LogPrint(BCLog::CJOIN, "OverviewPage::toggleCoinJoin -- Wallet is locked and user declined to unlock. Disabling CoinJoin.\n");
                return;
            }
        }
    }

    walletModel->toggleMixing();
    walletModel->updateTransaction();
    walletModel->setNumBlocks(std::numeric_limits<int>::max());
}

void OverviewPage::SetupTransactionList(int nNumItems) {
    ui->listTransactions->setMinimumHeight(nNumItems * (DECORATION_SIZE + 2));

    if(walletModel && walletModel->getOptionsModel()) {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(walletModel->getTransactionTableModel());
        filter->setLimit(nNumItems);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Status, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);
    }
}

void OverviewPage::DisableCoinJoinCompletely() {
    ui->toggleCoinJoin->setText("(" + tr("Disabled") + ")");
    ui->coinJoinReset->setText("(" + tr("Disabled") + ")");
    ui->frameCoinJoin->setEnabled(false);
    if (nWalletBackups <= 0) {
        ui->labelCoinJoinEnabled->setText("<span style='color:red;'>(" + tr("Disabled") + ")</span>");
    }
    walletModel->toggleMixing(true);
    walletModel->updateTransaction();
}
