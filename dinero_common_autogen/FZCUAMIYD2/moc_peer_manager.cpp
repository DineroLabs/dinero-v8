/****************************************************************************
** Meta object code from reading C++ file 'peer_manager.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../src/p2p/peer_manager.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'peer_manager.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.9.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN11PeerManagerE_t {};
} // unnamed namespace

template <> constexpr inline auto PeerManager::qt_create_metaobjectdata<qt_meta_tag_ZN11PeerManagerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "PeerManager",
        "p2pStarted",
        "",
        "boundPort",
        "peerReady",
        "Peer*",
        "peer",
        "messageFromPeer",
        "cmd",
        "payload",
        "startP2P",
        "stopP2P",
        "peerCount",
        "peers",
        "QList<Peer*>",
        "requestHeaders",
        "onNewConnection",
        "onPeerHandshake",
        "onPeerClosed",
        "reason",
        "onPeerMessage",
        "onRetryTimer"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'p2pStarted'
        QtMocHelpers::SignalData<void(quint16)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::UShort, 3 },
        }}),
        // Signal 'peerReady'
        QtMocHelpers::SignalData<void(Peer *)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Signal 'messageFromPeer'
        QtMocHelpers::SignalData<void(Peer *, QByteArray, QByteArray)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 }, { QMetaType::QByteArray, 8 }, { QMetaType::QByteArray, 9 },
        }}),
        // Slot 'startP2P'
        QtMocHelpers::SlotData<void()>(10, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'stopP2P'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'boundPort'
        QtMocHelpers::SlotData<quint16() const>(3, 2, QMC::AccessPublic, QMetaType::UShort),
        // Slot 'peerCount'
        QtMocHelpers::SlotData<int() const>(12, 2, QMC::AccessPublic, QMetaType::Int),
        // Slot 'peers'
        QtMocHelpers::SlotData<QList<Peer*>() const>(13, 2, QMC::AccessPublic, 0x80000000 | 14),
        // Slot 'requestHeaders'
        QtMocHelpers::SlotData<void(Peer *)>(15, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Slot 'onNewConnection'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onPeerHandshake'
        QtMocHelpers::SlotData<void(Peer *)>(17, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Slot 'onPeerClosed'
        QtMocHelpers::SlotData<void(Peer *, QString)>(18, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 5, 6 }, { QMetaType::QString, 19 },
        }}),
        // Slot 'onPeerMessage'
        QtMocHelpers::SlotData<void(Peer *, QByteArray, QByteArray)>(20, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 5, 6 }, { QMetaType::QByteArray, 8 }, { QMetaType::QByteArray, 9 },
        }}),
        // Slot 'onRetryTimer'
        QtMocHelpers::SlotData<void()>(21, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PeerManager, qt_meta_tag_ZN11PeerManagerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject PeerManager::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11PeerManagerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11PeerManagerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN11PeerManagerE_t>.metaTypes,
    nullptr
} };

void PeerManager::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PeerManager *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->p2pStarted((*reinterpret_cast< std::add_pointer_t<quint16>>(_a[1]))); break;
        case 1: _t->peerReady((*reinterpret_cast< std::add_pointer_t<Peer*>>(_a[1]))); break;
        case 2: _t->messageFromPeer((*reinterpret_cast< std::add_pointer_t<Peer*>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QByteArray>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QByteArray>>(_a[3]))); break;
        case 3: _t->startP2P(); break;
        case 4: _t->stopP2P(); break;
        case 5: { quint16 _r = _t->boundPort();
            if (_a[0]) *reinterpret_cast< quint16*>(_a[0]) = std::move(_r); }  break;
        case 6: { int _r = _t->peerCount();
            if (_a[0]) *reinterpret_cast< int*>(_a[0]) = std::move(_r); }  break;
        case 7: { QList<Peer*> _r = _t->peers();
            if (_a[0]) *reinterpret_cast< QList<Peer*>*>(_a[0]) = std::move(_r); }  break;
        case 8: _t->requestHeaders((*reinterpret_cast< std::add_pointer_t<Peer*>>(_a[1]))); break;
        case 9: _t->onNewConnection(); break;
        case 10: _t->onPeerHandshake((*reinterpret_cast< std::add_pointer_t<Peer*>>(_a[1]))); break;
        case 11: _t->onPeerClosed((*reinterpret_cast< std::add_pointer_t<Peer*>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        case 12: _t->onPeerMessage((*reinterpret_cast< std::add_pointer_t<Peer*>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QByteArray>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QByteArray>>(_a[3]))); break;
        case 13: _t->onRetryTimer(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
        case 1:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< Peer* >(); break;
            }
            break;
        case 2:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< Peer* >(); break;
            }
            break;
        case 8:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< Peer* >(); break;
            }
            break;
        case 10:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< Peer* >(); break;
            }
            break;
        case 11:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< Peer* >(); break;
            }
            break;
        case 12:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< Peer* >(); break;
            }
            break;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PeerManager::*)(quint16 )>(_a, &PeerManager::p2pStarted, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (PeerManager::*)(Peer * )>(_a, &PeerManager::peerReady, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (PeerManager::*)(Peer * , QByteArray , QByteArray )>(_a, &PeerManager::messageFromPeer, 2))
            return;
    }
}

const QMetaObject *PeerManager::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *PeerManager::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11PeerManagerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int PeerManager::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 14)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 14;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 14)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 14;
    }
    return _id;
}

// SIGNAL 0
void PeerManager::p2pStarted(quint16 _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void PeerManager::peerReady(Peer * _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void PeerManager::messageFromPeer(Peer * _t1, QByteArray _t2, QByteArray _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2, _t3);
}
QT_WARNING_POP
