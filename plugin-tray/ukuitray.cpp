/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * Copyright: 2011 Razor team
 * Authors:
 *   Alexander Sokoloff <sokoloff.a@gmail.com>
 *
 * Copyright: 2019 Tianjin KYLIN Information Technology Co., Ltd. *
 *
 * This program or library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 *
 * END_COMMON_COPYRIGHT_HEADER */

/********************************************************************
  Inspired by freedesktops tint2 ;)

*********************************************************************/

#include <QApplication>
#include <QDebug>
#include <QTimer>
#include <QtX11Extras/QX11Info>
#include "trayicon.h"
#include "../panel/iukuipanel.h"
#include "../panel/common/ukuigridlayout.h"
#include "ukuitray.h"
#include "xfitman.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xdamage.h>
#include <xcb/xcb.h>
#include <xcb/damage.h>
#undef Bool // defined as int in X11/Xlib.h

#include "../panel/iukuipanelplugin.h"
#include "traystorage.h"
//#include "../panel/customstyle.h"

#include <QPushButton>
#include <QToolButton>

#define _NET_SYSTEM_TRAY_ORIENTATION_HORZ 0
#define _NET_SYSTEM_TRAY_ORIENTATION_VERT 1

#define SYSTEM_TRAY_REQUEST_DOCK    0
#define SYSTEM_TRAY_BEGIN_MESSAGE   1
#define SYSTEM_TRAY_CANCEL_MESSAGE  2

#define XEMBED_EMBEDDED_NOTIFY  0
#define XEMBED_MAPPED          (1 << 0)


/************************************************

 ************************************************/

extern TrayStorageStatus storagestatus;

UKUITray::UKUITray(IUKUIPanelPlugin *plugin, QWidget *parent):
    QFrame(parent),
    mValid(false),
    mTrayId(0),
    mDamageEvent(0),
    mDamageError(0),
    mIconSize(TRAY_ICON_SIZE_DEFAULT, TRAY_ICON_SIZE_DEFAULT),
    mPlugin(plugin),
    mDisplay(QX11Info::display())
{
    mLayout = new UKUi::GridLayout(this);
    realign();
    _NET_SYSTEM_TRAY_OPCODE = XfitMan::atom("_NET_SYSTEM_TRAY_OPCODE");
    // Init the selection later just to ensure that no signals are sent until
    // after construction is done and the creating object has a chance to connect.
    QTimer::singleShot(0, this, SLOT(startTray()));
    QToolButton *bt=new QToolButton;
    bt->setStyle(new CustomStyle());
    bt->setIcon(QIcon("/usr/share/ukui-panel/panel/img/up.svg"));
    mLayout->addWidget(bt);

    tys= new TrayStorage();
    connect(bt,SIGNAL(clicked()),this,SLOT(storageBar()));
}


/************************************************
 ************************************************/
UKUITray::~UKUITray()
{
    stopTray();
}
void UKUITray::storageBar()
{
    int availableHeight = QGuiApplication::screens().at(0)->availableGeometry().height();
    int avaliableWidth=QGuiApplication::screens().at(0)->availableVirtualGeometry().width();

    switch (mPlugin->panel()->position()){
    case 0:
        tys->setGeometry(avaliableWidth-300,availableHeight-90,150,90);
        break;
    case 1:
        tys->setGeometry(avaliableWidth-300,0,130,90);
        break;
    case 2:
        tys->setGeometry(0,availableHeight-390,130,90);
        break;
    case 3:
        tys->setGeometry(avaliableWidth-130,availableHeight-290,130,90);
        break;
    default:
        break;
    }

    switch (storagestatus) {
    case HIDE:
        tys->show();
        storagestatus=HOVER;
        break;

    case HOVER:
        tys->hide();
        storagestatus=HIDE;
        break;
    default:
        break;
    }
}

/************************************************

 ************************************************/
bool UKUITray::nativeEventFilter(const QByteArray &eventType, void *message, long *)
{
    if (eventType != "xcb_generic_event_t")
        return false;

    xcb_generic_event_t* event = static_cast<xcb_generic_event_t *>(message);

    TrayIcon* icon;
    int event_type = event->response_type & ~0x80;

    switch (event_type)
    {
        case ClientMessage:
            clientMessageEvent(event);
            break;

//        case ConfigureNotify:
//            icon = findIcon(event->xconfigure.window);
//            if (icon)
//                icon->configureEvent(&(event->xconfigure));
//            break;

        case DestroyNotify: {
            unsigned long event_window;
            event_window = reinterpret_cast<xcb_destroy_notify_event_t*>(event)->window;
            icon = findIcon(event_window);
            if (icon)
            {
                icon->windowDestroyed(event_window);
                mIcons.removeAll(icon);
                delete icon;
            }
            break;
        }
        default:
            if (event_type == mDamageEvent + XDamageNotify)
            {
                xcb_damage_notify_event_t* dmg = reinterpret_cast<xcb_damage_notify_event_t*>(event);
                icon = findIcon(dmg->drawable);
                if (icon)
                    icon->update();
            }
            break;
    }

    return false;
}


/************************************************

 ************************************************/
void UKUITray::realign()
{
    mLayout->setEnabled(false);
    IUKUIPanel *panel = mPlugin->panel();

    if (panel->isHorizontal())
    {
        mLayout->setRowCount(panel->lineCount());
        mLayout->setColumnCount(0);
    }
    else
    {
        mLayout->setColumnCount(panel->lineCount());
        mLayout->setRowCount(0);
    }
    mLayout->setEnabled(true);
}


/************************************************

 ************************************************/
void UKUITray::clientMessageEvent(xcb_generic_event_t *e)
{
    unsigned long opcode;
    unsigned long message_type;
    Window id;
    xcb_client_message_event_t* event = reinterpret_cast<xcb_client_message_event_t*>(e);
    uint32_t* data32 = event->data.data32;
    message_type = event->type;
    opcode = data32[1];
    if(message_type != _NET_SYSTEM_TRAY_OPCODE)
        return;

    switch (opcode)
    {
        case SYSTEM_TRAY_REQUEST_DOCK:
            id = data32[2];
            if (id)
            {
                addIcon(id);
                storageAddIcon(id);
            }
            break;


        case SYSTEM_TRAY_BEGIN_MESSAGE:
        case SYSTEM_TRAY_CANCEL_MESSAGE:
            qDebug() << "we don't show balloon messages.";
            break;


        default:
//            if (opcode == xfitMan().atom("_NET_SYSTEM_TRAY_MESSAGE_DATA"))
//                qDebug() << "message from dockapp:" << e->data.b;
//            else
//                qDebug() << "SYSTEM_TRAY : unknown message type" << opcode;
            break;
    }
}

/************************************************

 ************************************************/
TrayIcon* UKUITray::findIcon(Window id)
{
    for(TrayIcon* icon : qAsConst(mIcons))
    {
        if (icon->iconId() == id || icon->windowId() == id)
            return icon;
    }
    return 0;
}


/************************************************

************************************************/
void UKUITray::setIconSize(QSize iconSize)
{
    mIconSize = iconSize;
    unsigned long size = qMin(mIconSize.width(), mIconSize.height());
    XChangeProperty(mDisplay,
                    mTrayId,
                    XfitMan::atom("_NET_SYSTEM_TRAY_ICON_SIZE"),
                    XA_CARDINAL,
                    32,
                    PropModeReplace,
                    (unsigned char*)&size,
                    1);
}


/************************************************

************************************************/
VisualID UKUITray::getVisual()
{
    VisualID visualId = 0;
    Display* dsp = mDisplay;

    XVisualInfo templ;
    templ.screen=QX11Info::appScreen();
    templ.depth=32;
    templ.c_class=TrueColor;

    int nvi;
    XVisualInfo* xvi = XGetVisualInfo(dsp, VisualScreenMask|VisualDepthMask|VisualClassMask, &templ, &nvi);

    if (xvi)
    {
        int i;
        XRenderPictFormat* format;
        for (i = 0; i < nvi; i++)
        {
            format = XRenderFindVisualFormat(dsp, xvi[i].visual);
            if (format &&
                format->type == PictTypeDirect &&
                format->direct.alphaMask)
            {
                visualId = xvi[i].visualid;
                break;
            }
        }
        XFree(xvi);
    }

    return visualId;
}


/************************************************
   freedesktop systray specification
 ************************************************/
void UKUITray::startTray()
{
    Display* dsp = mDisplay;
    Window root = QX11Info::appRootWindow();

    QString s = QString("_NET_SYSTEM_TRAY_S%1").arg(DefaultScreen(dsp));
    Atom _NET_SYSTEM_TRAY_S = XfitMan::atom(s.toLatin1());
    //this limit the tray apps  | will not run more Same apps
//    if (XGetSelectionOwner(dsp, _NET_SYSTEM_TRAY_S) != None)
//    {
//        qWarning() << "Another systray is running";
//        mValid = false;
//        return;
//    }

    // init systray protocol
    mTrayId = XCreateSimpleWindow(dsp, root, -1, -1, 1, 1, 0, 0, 0);

    XSetSelectionOwner(dsp, _NET_SYSTEM_TRAY_S, mTrayId, CurrentTime);
    if (XGetSelectionOwner(dsp, _NET_SYSTEM_TRAY_S) != mTrayId)
    {
        qWarning() << "Can't get systray manager";
        stopTray();
        mValid = false;
        return;
    }

    int orientation = _NET_SYSTEM_TRAY_ORIENTATION_HORZ;
    XChangeProperty(dsp,
                    mTrayId,
                    XfitMan::atom("_NET_SYSTEM_TRAY_ORIENTATION"),
                    XA_CARDINAL,
                    32,
                    PropModeReplace,
                    (unsigned char *) &orientation,
                    1);

    // ** Visual ********************************
    VisualID visualId = getVisual();
    if (visualId)
    {
        XChangeProperty(mDisplay,
                        mTrayId,
                        XfitMan::atom("_NET_SYSTEM_TRAY_VISUAL"),
                        XA_VISUALID,
                        32,
                        PropModeReplace,
                        (unsigned char*)&visualId,
                        1);
    }
    // ******************************************

    setIconSize(mIconSize);

    XClientMessageEvent ev;
    ev.type = ClientMessage;
    ev.window = root;
    ev.message_type = XfitMan::atom("MANAGER");
    ev.format = 32;
    ev.data.l[0] = CurrentTime;
    ev.data.l[1] = _NET_SYSTEM_TRAY_S;
    ev.data.l[2] = mTrayId;
    ev.data.l[3] = 0;
    ev.data.l[4] = 0;
    XSendEvent(dsp, root, False, StructureNotifyMask, (XEvent*)&ev);

    XDamageQueryExtension(mDisplay, &mDamageEvent, &mDamageError);
    mValid = true;

    qApp->installNativeEventFilter(this);
}


/************************************************

 ************************************************/
void UKUITray::stopTray()
{
    for (auto & icon : mIcons)
        disconnect(icon, &QObject::destroyed, this, &UKUITray::onIconDestroyed);
    qDeleteAll(mIcons);
    if (mTrayId)
    {
        XDestroyWindow(mDisplay, mTrayId);
        mTrayId = 0;
    }
    mValid = false;
}


/************************************************

 ************************************************/
void UKUITray::onIconDestroyed(QObject * icon)
{
    //in the time QOjbect::destroyed is emitted, the child destructor
    //is already finished, so the qobject_cast to child will return nullptr in all cases
    mIcons.removeAll(static_cast<TrayIcon *>(icon));
}

/************************************************

 ************************************************/
#include <QLabel>
void UKUITray::addIcon(Window winId)
{
    // decline to add an icon for a window we already manage
    TrayIcon *icon = findIcon(winId);
    if(icon)
        return;
    else
    icon = new TrayIcon(winId, mIconSize, this);

    if(xfitMan().getApplicationName(winId)=="kylin-nm" |xfitMan().getApplicationName(winId)=="ukui-volume-control-applet-qt" | xfitMan().getApplicationName(winId)=="ukui-flash-disk" | xfitMan().getApplicationName(winId)=="ukui-sidebar")
    {
    mIcons.append(icon);
    mLayout->addWidget(icon);
    }
    connect(icon, &QObject::destroyed, this, &UKUITray::onIconDestroyed);
}

void UKUITray::storageAddIcon(Window winId)
{
    TrayIcon *icon = findIcon(winId);
    if(icon)
        return;
    else
    icon = new TrayIcon(winId, mIconSize, this);
    if(xfitMan().getApplicationName(winId) !="kylin-nm"& xfitMan().getApplicationName(winId) !="ukui-flash-disk" &  xfitMan().getApplicationName(winId) !="ukui-volume-control-applet-qt" ){
    mIcons.append(icon);
    tys->mLayout->addWidget(icon);
    }
    connect(icon, &QObject::destroyed, tys, &TrayStorage::onIconDestroyed);
}
