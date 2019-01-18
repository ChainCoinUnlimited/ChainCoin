// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PROPOSALTABLEMODEL_H
#define BITCOIN_QT_PROPOSALTABLEMODEL_H

#include <qt/bitcoinunits.h>

#include <QAbstractTableModel>
#include <QStringList>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>

class ClientModel;
class ProposalTablePriv;

class ProposalTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit ProposalTableModel(ClientModel *parent = nullptr);
    ~ProposalTableModel();

    enum ColumnIndex {
        Amount = 0,
        StartDate = 1,
        EndDate = 2,
        YesVotes = 3,
        NoVotes = 4,
        AbsoluteYesVotes = 5,
        Proposal = 6,
        Endorsed = 7,
        Funding = 8,
        Percentage = 9
    };

    enum RoleIndex {
        ProposalHashRole = Qt::UserRole,
        ProposalUrlRole,
        ProposalDateFilterRole
    };

    int rowCount(const QModelIndex &parent) const;
    int columnCount(const QModelIndex &parent) const;
    QVariant data(const QModelIndex &index, int role) const;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const;
    QModelIndex index(int row, int column, const QModelIndex & parent = QModelIndex()) const;

private:
    ClientModel* const clientModel;
    ProposalTablePriv *priv = nullptr;
    QStringList columns;

    /** Notify listeners that data changed. */
    void emitDataChanged(int index);

public Q_SLOTS:
    /* New proposal, or proposal changed status */
    void updateProposal(const QString &hash, int status);

    friend class ProposalTablePriv;
};

#endif // BITCOIN_QT_PROPOSALTABLEMODEL_H
