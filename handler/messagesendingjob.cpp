/*
 * Copyright (C) 2016 Canonical, Ltd.
 *
 * Authors:
 *  Tiago Salem Herrmann <tiago.herrmann@canonical.com>
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

#include "accountentry.h"
#include "chatstartingjob.h"
#include "handlerdbus.h"
#include "messagesendingjob.h"
#include "telepathyhelper.h"
#include "texthandler.h"
#include <TelepathyQt/ContactManager>
#include <TelepathyQt/PendingContacts>
#include <QImage>

#define SMIL_TEXT_REGION "<region id=\"Text\" width=\"100%\" height=\"100%\" fit=\"scroll\" />"
#define SMIL_IMAGE_REGION "<region id=\"Image\" width=\"100%\" height=\"100%\" fit=\"meet\" />"
#define SMIL_VIDEO_REGION "<region id=\"Video\" width=\"100%\" height=\"100%\" fit=\"meet\" />"
#define SMIL_AUDIO_REGION "<region id=\"Audio\" width=\"100%\" height=\"100%\" fit=\"meet\" />"
#define SMIL_TEXT_PART "<par dur=\"3s\">\
       <text src=\"cid:%1\" region=\"Text\" />\
     </par>"
#define SMIL_IMAGE_PART "<par dur=\"5000ms\">\
       <img src=\"cid:%1\" region=\"Image\" />\
     </par>"
#define SMIL_VIDEO_PART "<par>\
       <video src=\"cid:%1\" region=\"Video\" />\
     </par>"
#define SMIL_AUDIO_PART "<par>\
       <audio src=\"cid:%1\" region=\"Audio\" />\
     </par>"

#define SMIL_FILE "<smil>\
   <head>\
     <layout>\
         %1\
     </layout>\
   </head>\
   <body>\
       %2\
   </body>\
 </smil>"

MessageSendingJob::MessageSendingJob(TextHandler *textHandler, PendingMessage message)
: MessageJob(textHandler), mTextHandler(textHandler), mMessage(message), mFinished(false)
{
    qDebug() << __PRETTY_FUNCTION__;
    static ulong count = 0;
    // just to avoid overflowing
    if (count == ULONG_MAX) {
        count = 0;
    }
    mObjectPath = HandlerDBus::instance()->registerObject(this, QString("messagesendingjob%1").arg(count++));
}

MessageSendingJob::~MessageSendingJob()
{
    qDebug() << __PRETTY_FUNCTION__;
    HandlerDBus::instance()->unregisterObject(mObjectPath);
}

QString MessageSendingJob::accountId() const
{
    qDebug() << __PRETTY_FUNCTION__;
    return mAccountId;
}

QString MessageSendingJob::channelObjectPath() const
{
    qDebug() << __PRETTY_FUNCTION__;
    return mChannelObjectPath;
}

QString MessageSendingJob::objectPath() const
{
    qDebug() << __PRETTY_FUNCTION__;
    return mObjectPath;
}

void MessageSendingJob::startJob()
{
    qDebug() << __PRETTY_FUNCTION__;
    qDebug() << "Getting account for id:" << mMessage.accountId;
    AccountEntry *account = TelepathyHelper::instance()->accountForId(mMessage.accountId);
    if (!account) {
        setStatus(Failed);
        scheduleDeletion();
    }

    setStatus(Running);

    // check if the message should be sent via multimedia account
    // we just use fallback to 1-1 chats
    if (account->type() == AccountEntry::PhoneAccount) {
        Q_FOREACH(AccountEntry *newAccount, TelepathyHelper::instance()->accounts()) {
            // TODO: we have to find the multimedia account that matches the same phone number,
            // but for now we just pick any multimedia connected account
            if (newAccount->type() != AccountEntry::MultimediaAccount) {
                continue;
            }
            // FIXME: the fallback implementation needs to be changed to use protocol info and create a map of
            // accounts. Also, it needs to check connection capabilities to determine if we can send message
            // to offline contacts.
            bool shouldFallback = true;
            // if the account is offline, dont fallback to this account
            if (!newAccount->connected()) {
                continue;
            }
            QList<Tp::TextChannelPtr> channels = mTextHandler->existingChannels(newAccount->accountId(), mMessage.properties);
            // check if we have a channel for this contact already and get the contact pointer from there,
            // this way we avoid doing the while(op->isFinished()) all the time
            if (!channels.isEmpty()) {
                // FIXME: we need to re-evaluate the rules to fallback
                // if the contact is known, force fallback to this account
                Q_FOREACH(const Tp::ContactPtr &contact, channels.first()->groupContacts(false)) {
                    Tp::Presence presence = contact->presence();
                    shouldFallback = (presence.type() == Tp::ConnectionPresenceTypeAvailable ||
                                      presence.type() == Tp::ConnectionPresenceTypeOffline);
                    if (!shouldFallback) {
                        break;
                    }
                }
            } else {
                QStringList participantIds = mMessage.properties["participantIds"].toStringList();
                Tp::PendingContacts *op = newAccount->account()->connection()->contactManager()->contactsForIdentifiers(participantIds);
                while (!op->isFinished()) {
                    qApp->processEvents();
                }
                Q_FOREACH(const Tp::ContactPtr &contact, op->contacts()) {
                    Tp::Presence presence = contact->presence();
                    shouldFallback = (presence.type() == Tp::ConnectionPresenceTypeAvailable ||
                                      presence.type() == Tp::ConnectionPresenceTypeOffline);
                    if (!shouldFallback) {
                        break;
                    }
                }
            }
            if (shouldFallback) {
                account = newAccount;
                break;
            }
        }
    }

    // save the account
    mAccount = account;
    setAccountId(mAccount->accountId());

    if (!account->connected()) {
        connect(account, &AccountEntry::connectedChanged, [this, account]() {
            if (account->connected()) {
                findOrCreateChannel();
            }
        });
        return;
    }

    findOrCreateChannel();
}

void MessageSendingJob::findOrCreateChannel()
{
    qDebug() << __PRETTY_FUNCTION__;
    // now that we know what account to use, find existing channels or request a new one
    QList<Tp::TextChannelPtr> channels = mTextHandler->existingChannels(mAccount->accountId(), mMessage.properties);
    if (channels.isEmpty()) {
        ChatStartingJob *job = new ChatStartingJob(mTextHandler, mAccount->accountId(), mMessage.properties);
        connect(job, &MessageJob::finished, [this, job]() {
            if (job->status() == MessageJob::Failed) {
                setStatus(Failed);
                scheduleDeletion();
                return;
            }

            mTextChannel = job->textChannel();
            sendMessage();
        });
        job->startJob();
        return;
    }

    mTextChannel = channels.last();
    sendMessage();
}

void MessageSendingJob::sendMessage()
{
    qDebug() << __PRETTY_FUNCTION__;
    Tp::PendingSendMessage *op = mTextChannel->send(buildMessage(mMessage));
    connect(op, &Tp::PendingOperation::finished, [this, op]() {
        if (op->isError()) {
            setStatus(Failed);
            scheduleDeletion();
            return;
        }

        setChannelObjectPath(mTextChannel->objectPath());
        setStatus(Finished);
        scheduleDeletion();
    });
}

void MessageSendingJob::setAccountId(const QString &accountId)
{
    qDebug() << __PRETTY_FUNCTION__;
    mAccountId = accountId;
    Q_EMIT accountIdChanged();
}

void MessageSendingJob::setChannelObjectPath(const QString &objectPath)
{
    qDebug() << __PRETTY_FUNCTION__;
    mChannelObjectPath = objectPath;
    Q_EMIT channelObjectPathChanged();
}

Tp::MessagePartList MessageSendingJob::buildMessage(const PendingMessage &pendingMessage)
{
    qDebug() << __PRETTY_FUNCTION__;
    Tp::MessagePartList message;
    Tp::MessagePart header;
    QString smil, regions, parts;
    bool hasImage = false, hasText = false, hasVideo = false, hasAudio = false, isMMS = false;

    AccountEntry *account = TelepathyHelper::instance()->accountForId(pendingMessage.accountId);
    if (!account) {
        // account does not exist
        return Tp::MessagePartList();
    }

    bool temporaryFiles = (pendingMessage.properties.contains("x-canonical-tmp-files") &&
                           pendingMessage.properties["x-canonical-tmp-files"].toBool());

    // add the remaining properties to the message header
    QVariantMap::const_iterator it = pendingMessage.properties.begin();
    for (; it != pendingMessage.properties.end(); ++it) {
        header[it.key()] = QDBusVariant(it.value());
    }

    // check if this message should be sent as an MMS
    if (account->type() == AccountEntry::PhoneAccount) {
        isMMS = (pendingMessage.attachments.size() > 0 ||
                 (header.contains("x-canonical-mms") && header["x-canonical-mms"].variant().toBool()) ||
                 (pendingMessage.properties["participantIds"].toStringList().size() > 1 && TelepathyHelper::instance()->mmsGroupChat()));
        if (isMMS) {
            header["x-canonical-mms"] = QDBusVariant(true);
        }
    }

    // this flag should not be in the message header, it's only useful for the handler
    header.remove("x-canonical-tmp-files");
    header.remove("chatType");
    header.remove("threadId");
    header.remove("participantIds");

    header["message-type"] = QDBusVariant(0);
    message << header;

    // convert AttachmentList struct into telepathy Message parts
    Q_FOREACH(const AttachmentStruct &attachment, pendingMessage.attachments) {
        QByteArray fileData;
        QString newFilePath = QString(attachment.filePath).replace("file://", "");
        QFile attachmentFile(newFilePath);
        if (!attachmentFile.open(QIODevice::ReadOnly)) {
            qWarning() << "fail to load attachment" << attachmentFile.errorString() << attachment.filePath;
            continue;
        }
        if (attachment.contentType.startsWith("image/")) {
            if (isMMS) {
                hasImage = true;
                parts += QString(SMIL_IMAGE_PART).arg(attachment.id);
                // check if we need to reduce de image size in case it's bigger than 300k
                // this check is only valid for MMS
                if (attachmentFile.size() > 307200) {
                    QImage scaledImage(newFilePath);
                    if (!scaledImage.isNull()) {
                        QBuffer buffer(&fileData);
                        buffer.open(QIODevice::WriteOnly);
                        scaledImage.scaled(640, 640, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "jpg");
                    }
                } else {
                    fileData = attachmentFile.readAll();
                }
            }
        } else if (attachment.contentType.startsWith("video/")) {
            if (isMMS) {
                hasVideo = true;
                parts += QString(SMIL_VIDEO_PART).arg(attachment.id);
            }
        } else if (attachment.contentType.startsWith("audio/")) {
            if (isMMS) {
                hasAudio = true;
                parts += QString(SMIL_AUDIO_PART).arg(attachment.id);
            }
        } else if (attachment.contentType.startsWith("text/plain")) {
            if (isMMS) {
                hasText = true;
                parts += QString(SMIL_TEXT_PART).arg(attachment.id);
            }
        } else if (attachment.contentType.startsWith("text/vcard") ||
                   attachment.contentType.startsWith("text/x-vcard")) {
        } else if (isMMS) {
            // for MMS we just support the contentTypes above
            if (temporaryFiles) {
                attachmentFile.remove();
            }
            continue;
        }

        if (fileData.isEmpty()) {
            fileData = attachmentFile.readAll();
        }

        if (temporaryFiles) {
            attachmentFile.remove();
        }

        if (hasVideo) {
            regions += QString(SMIL_VIDEO_REGION);
        }

        if (hasAudio) {
            regions += QString(SMIL_AUDIO_REGION);
        }

        if (hasText) {
            regions += QString(SMIL_TEXT_REGION);
        }
        if (hasImage) {
            regions += QString(SMIL_IMAGE_REGION);
        }

        Tp::MessagePart part;
        part["content-type"] =  QDBusVariant(attachment.contentType);
        part["identifier"] = QDBusVariant(attachment.id);
        part["content"] = QDBusVariant(fileData);
        part["size"] = QDBusVariant(fileData.size());

        message << part;
    }

    if (!pendingMessage.message.isEmpty()) {
        Tp::MessagePart part;
        QString tmpTextId("text_0.txt");
        part["content-type"] =  QDBusVariant(QString("text/plain"));
        part["identifier"] = QDBusVariant(tmpTextId);
        part["content"] = QDBusVariant(pendingMessage.message);
        part["size"] = QDBusVariant(pendingMessage.message.size());
        if (isMMS) {
            parts += QString(SMIL_TEXT_PART).arg(tmpTextId);
            regions += QString(SMIL_TEXT_REGION);
        }
        message << part;
    }

    if (isMMS) {
        Tp::MessagePart smilPart;
        smil = QString(SMIL_FILE).arg(regions).arg(parts);
        smilPart["content-type"] =  QDBusVariant(QString("application/smil"));
        smilPart["identifier"] = QDBusVariant(QString("smil.xml"));
        smilPart["content"] = QDBusVariant(smil);
        smilPart["size"] = QDBusVariant(smil.size());

        message << smilPart;
    }

    return message;
}


