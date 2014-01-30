/*
 * Copyright (C) 2012-2013 Canonical, Ltd.
 *
 * Authors:
 *  Gustavo Pichorim Boiko <gustavo.boiko@canonical.com>
 *
 * This file is part of telephony-service.
 *
 * telephony-service is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * telephony-service is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TEXTHANDLER_H
#define TEXTHANDLER_H

#include <QtCore/QObject>
#include <QtCore/QMap>
#include <TelepathyQt/TextChannel>
#include <TelepathyQt/ReceivedMessage>

class TextHandler : public QObject
{
    Q_OBJECT
public:
    static TextHandler *instance();
    void startChat(const QStringList &phoneNumber);
    void startChat(const Tp::Contacts &contacts);

public Q_SLOTS:
    void sendMessage(const QStringList &phoneNumber, const QString &message);
    void acknowledgeMessages(const QStringList &phoneNumber, const QStringList &messageIds);

protected Q_SLOTS:
    void onTextChannelAvailable(Tp::TextChannelPtr channel);
    void onContactsAvailable(Tp::PendingOperation *op);
    void onMessageSent(Tp::PendingOperation *op);
    void onConnectedChanged();

protected:
    Tp::TextChannelPtr existingChat(const QStringList &phoneNumber);

private:
    explicit TextHandler(QObject *parent = 0);

    QList<Tp::TextChannelPtr> mChannels;
    QMap<QString, Tp::ContactPtr> mContacts;
    QMap<QStringList, QStringList> mPendingMessages;
};

#endif // TEXTHANDLER_H
