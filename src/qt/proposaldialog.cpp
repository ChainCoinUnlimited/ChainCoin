// Copyright (c) 2019 PM-Tech
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/proposaldialog.h>
#include <qt/forms/ui_proposaldialog.h>

#include <qt/addressbookpage.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>

#include <string>
#include <vector>

#include <QClipboard>

static const int MAX_DATA_SIZE = 256;

ProposalDialog::ProposalDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ProposalDialog),
    clientModel(nullptr),
    model(nullptr),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    QString theme = GUIUtil::getThemeName();

#ifdef Q_OS_MAC // Icons on push buttons are very uncommon on Mac
    ui->submitButton->setIcon(QIcon());
    ui->clearButton->setIcon(QIcon());
#else
    ui->submitButton->setIcon(QIcon(":/icons/" + theme + "/edit"));
    ui->clearButton->setIcon(QIcon(":/icons/" + theme + "/remove"));
#endif

    // These icons are needed on Mac also
    ui->addressBookButton->setIcon(QIcon(":/icons/" + theme + "/address-book"));
    ui->pasteButton->setIcon(QIcon(":/icons/" + theme + "/editpaste"));

    GUIUtil::setupAddressWidget(ui->addressIn, this);

    ui->dateEdit->setDate(QDate::currentDate());
    ui->dateEdit->setMinimumDate(QDate::currentDate());

    ui->addressIn->installEventFilter(this);
    ui->proposalIn->installEventFilter(this);
    ui->amountIn->installEventFilter(this);
}

ProposalDialog::~ProposalDialog()
{
    delete ui;
}

void ProposalDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;
}

void ProposalDialog::setModel(WalletModel *_model)
{
    this->model = _model;
}

void ProposalDialog::setAddress(const QString &address)
{
    ui->addressIn->setText(address);
    ui->proposalIn->setFocus();
}

void ProposalDialog::on_addressBookButton_clicked()
{
    if (model && model->getAddressTableModel())
    {
        AddressBookPage dlg(platformStyle, AddressBookPage::ForSelection, AddressBookPage::ReceivingTab, this);
        dlg.setModel(model->getAddressTableModel());
        if (dlg.exec())
        {
            setAddress(dlg.getReturnValue());
        }
    }
}

void ProposalDialog::on_pasteButton_clicked()
{
    setAddress(QApplication::clipboard()->text());
}

void ProposalDialog::on_submitButton_clicked()
{
    if (!clientModel) return;
    if (!model) return;

    CTxDestination destination = DecodeDestination(ui->addressIn->text().toStdString());
    if (!IsValidDestination(destination)) {
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("The entered address is invalid.") + QString(" ") + tr("Please check the address and try again."));
        return;
    }

    QUrl url(QUrl::fromUserInput(ui->urlIn->text()));
    if (!url.isValid()) {
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("The entered URL is invalid.") + QString(" ") + tr("Please check the URL and try again."));
        return;
    }

    ui->urlIn->setText(url.toDisplayString(QUrl::FullyDecoded));

    QDate date(ui->dateEdit->date());
    QDateTime dt = QDateTime::fromString(date.toString() + " 00:00:00");
    int64_t startEpoch = dt.toTime_t();
    int64_t endEpoch = startEpoch + ui->spinBox->value() * Params().GetConsensus().nSuperblockCycle * Params().GetConsensus().nPowTargetSpacing;

    CAmount amount = 0;
    std::string errRet = "";
    std::string name = ui->proposalIn->toPlainText().toStdString();

    for (size_t i = 0; i < name.length(); i++){
        if (isspace(name[i])) name[i] = '_';
    }


    std::string data = "[[\"proposal\",{\"end_epoch\":" + std::to_string(endEpoch)
            + ",\"name\":\"" + name
            + "\",\"payment_address\":\"" + ui->addressIn->text().toStdString()
            + "\",\"payment_amount\":" + std::to_string(ui->amountIn->value() / COIN)
            + ",\"start_epoch\":" + std::to_string(startEpoch)
            + ",\"type\":1,\"url\":\"" + url.toDisplayString(QUrl::FullyDecoded).toStdString()
            + "\"}]]";

    uint256 hash = clientModel->node().validateProposal(data, uint256(), amount, errRet);

    if (hash == uint256()) {
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("The entered Proposal is invalid: ") + QString::fromStdString(errRet));
        return;
    }

    WalletModel::UnlockContext ctx(model->requestUnlock());
    if (!ctx.isValid())
    {
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("Wallet unlock was cancelled."));
        return;
    }

    QList<SendCoinsRecipient> recipients;
    // prepare transaction for getting txFee earlier
    WalletModelTransaction currentTransaction(recipients);
    WalletModel::SendCoinsReturn prepareStatus;

    // Always use a CCoinControl instance, use the CoinControlDialog instance if CoinControl has been enabled
    CCoinControl ctrl;
    if (model->getOptionsModel()->getCoinControlFeatures())
        ctrl = *CoinControlDialog::coinControl();

    prepareStatus = model->prepareCollateral(currentTransaction, ctrl, hash, amount);

    // process prepareStatus and on error generate message shown to user
    processSendCoinsReturn(prepareStatus,
        BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), currentTransaction.getTransactionFee()));

    if(prepareStatus.status != WalletModel::OK) {
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("Error creating collateral transaction. Please check your wallet balance and make sure your wallet is unlocked."));
        return;
    }

    // now send the prepared transaction
    WalletModel::SendCoinsReturn sendStatus = model->sendCoins(currentTransaction);
    // process sendStatus and on error generate message shown to user
    processSendCoinsReturn(sendStatus);

    if (sendStatus.status == WalletModel::OK) {
        CoinControlDialog::coinControl()->UnSelectAll();
        Q_EMIT coinsSent(currentTransaction.getWtx()->get().GetHash());
    }

    // and finalize our proposal
    hash = clientModel->node().validateProposal(data, currentTransaction.getWtx()->get().GetHash(), amount, errRet);

    if(hash == uint256()) {
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("Error creating collateral proposal: ") + QString::fromStdString(errRet));
        return;
    }

    ui->statusLabel->setStyleSheet("QLabel { color: green; }");
    ui->statusLabel->setText(QString("<nobr>") + tr("Proposal OK, Hash: ") + QString::fromStdString(hash.ToString()) + QString("</nobr>"));

    ui->addressIn->setPlaceholderText(ui->addressIn->text());
    ui->urlIn->setPlaceholderText(url.toDisplayString(QUrl::FullyDecoded));
    ui->proposalIn->setPlaceholderText(ui->proposalIn->toPlainText());

    ui->addressIn->clear();
    ui->urlIn->clear();
    ui->proposalIn->clear();
    ui->amountIn->clear();

    ui->addressIn->setFocus();
}

void ProposalDialog::processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to SendCoinsDialog usage of WalletModel::SendCoinsReturn.
    // WalletModel::TransactionCommitFailed is used only in WalletModel::sendCoins()
    // all others are used only in WalletModel::prepareTransaction()
    switch(sendCoinsReturn.status)
    {
    case WalletModel::InvalidAddress:
        msgParams.first = tr("The recipient address is not valid. Please recheck.");
        break;
    case WalletModel::InvalidAmount:
        msgParams.first = tr("The amount to pay must be larger than 0.");
        break;
    case WalletModel::AmountExceedsBalance:
        msgParams.first = tr("The amount exceeds your balance.");
        break;
    case WalletModel::AmountWithFeeExceedsBalance:
        msgParams.first = tr("The total exceeds your balance when the %1 transaction fee is included.").arg(msgArg);
        break;
    case WalletModel::DuplicateAddress:
        msgParams.first = tr("Duplicate address found: addresses should only be used once each.");
        break;
    case WalletModel::TransactionCreationFailed:
        msgParams.first = tr("Transaction creation failed!");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::TransactionCommitFailed:
        msgParams.first = tr("The transaction was rejected with the following reason: %1").arg(sendCoinsReturn.reasonCommitFailed);
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::AbsurdFee:
        msgParams.first = tr("A fee higher than %1 is considered an absurdly high fee.").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->wallet().getDefaultMaxTxFee()));
        break;
    case WalletModel::PaymentRequestExpired:
        msgParams.first = tr("Payment request expired.");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    // included to prevent a compiler warning.
    case WalletModel::OK:
    default:
        return;
    }

    Q_EMIT message(tr("Send Collateral"), msgParams.first, msgParams.second);
}

void ProposalDialog::on_clearButton_clicked()
{
    ui->addressIn->clear();
    ui->urlIn->clear();
    ui->proposalIn->clear();
    ui->amountIn->clear();
    ui->statusLabel->clear();

    ui->addressIn->setFocus();
}

// close button
void ProposalDialog::on_closeButton_clicked(QAbstractButton *button)
{
    if (ui->closeButton->buttonRole(button) == QDialogButtonBox::RejectRole)
        done(QDialog::Accepted); // closes the dialog
}

void ProposalDialog::on_proposalIn_textChanged()
{
    if(ui->proposalIn->toPlainText().length() > MAX_DATA_SIZE)
    {
        int diff = ui->proposalIn->toPlainText().length() - MAX_DATA_SIZE;
        QString newStr = ui->proposalIn->toPlainText();
        newStr.chop(diff);
        ui->proposalIn->setPlainText(newStr);
        QTextCursor cursor(ui->proposalIn->textCursor());
        cursor.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);
        ui->proposalIn->setTextCursor(cursor);
    }
}
