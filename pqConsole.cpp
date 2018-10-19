/*  Part of SWI-Prolog interface to Qt

    Author:        Carlo Capelli
    E-mail:        cc.carlo.cap@gmail.com
    Copyright (c)  2013-2015, Carlo Capelli
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in
       the documentation and/or other materials provided with the
       distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

// by now peek system namespace. Eventually, will move to pqConsole
#define PROLOG_MODULE "system"

#include <SWI-Stream.h>

#include "Swipl_IO.h"
#include "pqConsole.h"
#include "PREDICATE.h"
#include "do_events.h"
#include "ConsoleEdit.h"
#include "Preferences.h"
#include "pqMainWindow.h"

#include <QTime>
#include <QStack>
#include <QDebug>
#include <QMenuBar>
#include <QClipboard>
#include <QFileDialog>
#include <QGridLayout>
#include <QColorDialog>
#include <QFontDialog>
#include <QMessageBox>
#include <QMetaObject>
#include <QMainWindow>
#include <QApplication>
#include <QFontMetrics>
#include <QMetaProperty>

/** Run a default GUI to demo the ability to embed Prolog with minimal effort.
 *  It will evolve - eventually - from a demo
 *  to the *official* SWI-Prolog console in main distribution - Wow
 */
int pqConsole::runDemo(int argc, char *argv[]) {
    QApplication a(argc, argv);
    pqMainWindow w(argc, argv);
    w.show();
    return a.exec();
}

/** standard constructor, generated by QtCreator.
 */
pqConsole::pqConsole() {
}

/** depth first search of widgets hierarchy, from application topLevelWidgets
 */
static QWidget *search_widget(std::function<bool(QWidget* w)> match) {
    foreach (auto widget, QApplication::topLevelWidgets()) {
        QStack<QObject*> s;
        s.push(widget);
        while (!s.isEmpty()) {
            auto p = qobject_cast<QWidget*>(s.pop());
            if (match(p))
                return p;
            foreach (auto c, p->children())
                if (c->isWidgetType())
                    s.push(c);
        }
    }
    return 0;
}

/** search widgets hierarchy looking for the first (the only)
 *  that owns the calling thread ID
 */
static ConsoleEdit *console_by_thread() {
    int thid = PL_thread_self();
    return qobject_cast<ConsoleEdit*>(search_widget([=](QWidget* p) {
        if (auto ce = qobject_cast<ConsoleEdit*>(p))
            return ce->match_thread(thid);
        return false;
    }));
}

/** search widgets hierarchy looking for any ConsoleEdit
 */
static ConsoleEdit *console_peek_first() {
    return qobject_cast<ConsoleEdit*>(search_widget([](QWidget* p) {
        return qobject_cast<ConsoleEdit*>(p) != 0;
    }));
}

/** unify a property of QObject:
 *  allows read/write of simple atomic values
 */
static QString unify(const QMetaProperty& p, QObject *o, PlTerm v) {

    #define OK return QString()

    switch (v.type()) {

    case PL_VARIABLE:
        switch (p.type()) {
        case QVariant::Bool:
            v = p.read(o).toBool() ? A("true") : A("false");
            OK;
        case QVariant::Int:
            if (p.isEnumType()) {
                Q_ASSERT(!p.isFlagType());  // TBD
                QMetaEnum e = p.enumerator();
                if (CCP key = e.valueToKey(p.read(o).toInt())) {
                    v = A(key);
                    OK;
                }
            }
            v = long(p.read(o).toInt());
            OK;
        case QVariant::UInt:
            v = long(p.read(o).toUInt());
            OK;
        case QVariant::String:
            v = A(p.read(o).toString());
            OK;
        default:
            break;
        }
        break;

    case PL_INTEGER:
        switch (p.type()) {
        case QVariant::Int:
        case QVariant::UInt:
            if (p.write(o, qint32(v)))
                OK;
        default:
            break;
        }
        break;

    case PL_ATOM:
        switch (p.type()) {
        case QVariant::String:
            if (p.write(o, t2w(v)))
                OK;
	    break;
        case QVariant::Int:
            if (p.isEnumType()) {
                Q_ASSERT(!p.isFlagType());  // TBD
                int i = p.enumerator().keyToValue(v);
                if (i != -1) {
                    p.write(o, i);
                    OK;
                }
            }
        default:
            break;
        }
        break;

    case PL_FLOAT:
        switch (p.type()) {
        case QVariant::Double:
            if (p.write(o, double(v)))
                OK;
        default:
            break;
        }
        break;

    default:
        break;
    }

    return o->tr("property %1: type mismatch").arg(p.name());
}

/** unify a property of QObject, seek by name:
 *  allows read/write of basic atomic values (note: enums are symbolics)
 */
static QString unify(CCP name, QObject *o, PlTerm v) {
    int pid = o->metaObject()->indexOfProperty(name);
    if (pid >= 0)
        return unify(o->metaObject()->property(pid), o, v);
    return o->tr("property %1: not found").arg(name);
}

// SWIPL-WIN.EXE interface implementation

/** window_title(-Old, +New)
 *  get/set console title
 */
PREDICATE(window_title, 2) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        QWidget *w = c->parentWidget();
        if (qobject_cast<QMainWindow*>(w)) {
            PL_A1 = A(w->windowTitle());
            w->setWindowTitle(t2w(PL_A2));
            return TRUE;
        }
    }
    return FALSE;
}

/** win_window_pos(Options)
 *  Option:
 *     size(W, H)
 *     position(X, Y)
 *     zorder(ZOrder)
 *     show(Bool)
 *     activate
 */
PREDICATE(win_window_pos, 1) {
    ConsoleEdit* c = console_by_thread();
    if (!c)
        return FALSE;

    QWidget *w = c->parentWidget();
    if (!w)
        return FALSE;

    T opt;
    L options(PL_A1);
    typedef QPair<int, QString> O;
    while (options.next(opt)) {
        O o = O(opt.arity(), opt.name());
        if (o == O(2, "size")) {
            long W = opt[1], H = opt[2];
            QSize sz = c->fontMetrics().size(0, "Q");
            w->resize(sz.width() * W, sz.height() * H);
            continue;
        }
        if (o == O(2, "position")) {
            long X = opt[1], Y = opt[2];
            w->move(X, Y);
            continue;
        }
        if (o == O(1, "zorder")) {
            // TBD ...
            // long ZOrder = opt[1];
            continue;
        }
        if (o == O(1, "show")) {
            bool y = QString(opt[1].name()) == "true";
            if (y)
                w->show();
            else
                w->hide();
            continue;
        }
        if (o == O(0, "activate")) {
            w->activateWindow();
            continue;
        }

        // print_error
        return FALSE;
    }

    return TRUE;
}

/** win_has_menu
 *  true =only= when ConsoleEdit is directly framed inside a QMainWindow
 */
PREDICATE0(win_has_menu) {
    auto ce = console_by_thread();
    return ce && qobject_cast<QMainWindow*>(ce->parentWidget()) ? TRUE : FALSE;
}

/** MENU interface
 *  helper to lookup position and issue action creation
 */
/** win_insert_menu(+Label, +Before)
 *  do action construction
 */
PREDICATE(win_insert_menu, 2) {
    if (ConsoleEdit *ce = console_by_thread()) {
        QString Label = t2w(PL_A1), Before = t2w(PL_A2);
        ce->exec_func([=]() {
            if (auto mw = qobject_cast<QMainWindow*>(ce->parentWidget())) {
                auto mbar = mw->menuBar();
                foreach (QAction *ac, mbar->actions())
                    if (ac->text() == Label)
                        return;
                foreach (QAction *ac, mbar->actions())
                    if (ac->text() == Before) {
                        mbar->insertMenu(ac, new QMenu(Label));
                        return;
                    }
                if (Before == "-") {
                    mbar->addMenu(Label);
                    return;
                }
            }
            qDebug() << "failed win_insert_menu" << Label << Before;
        });
        return TRUE;
    }
    return FALSE;
}

/** win_insert_menu_item(+Pulldown, +Label, +Before, :Goal)
 *  does search insertion position and create menu item
 */
PREDICATE(win_insert_menu_item, 4) {

    if (ConsoleEdit *ce = console_by_thread()) {
        QString Pulldown = t2w(PL_A1), Label, Before = t2w(PL_A3), Goal;
        QList<QPair<QString, QString>> lab_act;

        if (PL_A2.arity() == 2 /* &&
            strcmp(PL_A2.name(), "/") == 0 &&
            PL_A2[2].type() == PL_LIST &&
            PL_A4.type() == PL_LIST */ )
        {
            Label = t2w(PL_A2[1]);
            PlTail labels(PL_A2[2]), actions(PL_A4);
            PlTerm label, action;
            while (labels.next(label) && actions.next(action))
                lab_act.append(qMakePair(t2w(label), t2w(action)));
        }
        else {
            Label = t2w(PL_A2);
            Goal = t2w(PL_A4);
        }

        QString ctxtmod = t2w(PlAtom(PL_module_name(PL_context())));
        // if (PlCall("context_module", cx)) ctxtmod = t2w(cx); -- same as above: system
        ctxtmod = "win_menu";

        ce->exec_func([=]() {
            if (auto mw = qobject_cast<pqMainWindow*>(ce->parentWidget())) {
                foreach (QAction *ac, mw->menuBar()->actions())
                    if (ac->text() == Pulldown) {
                        QMenu *mn = ac->menu();
                        if (!lab_act.isEmpty()) {
                            foreach (QAction *cm, mn->actions())
                                if (cm->text() == Label) {
                                    cm->setMenu(new QMenu(Label));
                                    foreach (auto p, lab_act)
                                        mw->addActionPq(ce, cm->menu(), p.first, p.second);
                                    return;
                                }
                            return;
                        }
                        else {
                            if (Label != "--")
                                foreach (QAction *bc, mn->actions())
                                    if (bc->text() == Label) {
                                        bc->setToolTip(Goal);
                                        return;
                                    }
                            if (Before == "-") {
                                if (Label == "--")
                                    mn->addSeparator();
                                else
                                    mw->add_action(ce, mn, Label, ctxtmod, Goal);
                                return;
                            }
                            foreach (QAction *bc, mn->actions())
                                if (bc->text() == Before) {
                                    if (Label == "--")
                                        mn->insertSeparator(bc);
                                    else
                                        mw->add_action(ce, mn, Label, ctxtmod, Goal, bc);
                                    return;
                                }

                            QAction *bc = mw->add_action(ce, mn, Before, ctxtmod, "");
                            mw->add_action(ce, mn, Label, ctxtmod, Goal, bc);
                        }
                    }
            }
        });
        return TRUE;
    }
    return FALSE;
}

/** tty_clear
 *  as requested by Annie. Should as well be implemented capturing ANSI terminal sequence
 */
PREDICATE0(tty_clear) {
    ConsoleEdit* c = console_by_thread();
    if (c) {

        // loqt does better...
        // pqConsole::gui_run([&]() { c->tty_clear(); });

        ConsoleEdit::exec_sync s;
        c->exec_func([&]() {
            c->tty_clear();
            s.go();
        });
        s.stop();

        // buggy - need to sync
        // c->tty_clear();

        return TRUE;
    }
    return FALSE;
}

/** win_open_console(Title, In, Out, Err, [ registry_key(Key) ])
 *  code stolen - verbatim - from pl-ntmain.c
 *  registry_key(Key) unused by now
 */
PREDICATE(win_open_console, 5) {

    qDebug() << "win_open_console" << CVP(QThread::currentThread());

    ConsoleEdit *ce = console_peek_first();
    if (!ce)
        throw PlException(A("no ConsoleEdit available"));

    static IOFUNCTIONS rlc_functions = {
        Swipl_IO::_read_f,
        Swipl_IO::_write_f,
        Swipl_IO::_seek_f,
        Swipl_IO::_close_f,
        Swipl_IO::_control_f,
        Swipl_IO::_seek64_f
    };

    #define STREAM_COMMON (\
        SIO_TEXT|       /* text-stream */           \
        SIO_NOCLOSE|    /* do no close on abort */	\
        SIO_ISATTY|     /* terminal */              \
        SIO_NOFEOF)     /* reset on end-of-file */

    auto c = new Swipl_IO;
    IOSTREAM
        *in  = Snew(c,  SIO_INPUT|SIO_LBUF|STREAM_COMMON, &rlc_functions),
        *out = Snew(c, SIO_OUTPUT|SIO_LBUF|STREAM_COMMON, &rlc_functions),
        *err = Snew(c, SIO_OUTPUT|SIO_NBUF|STREAM_COMMON, &rlc_functions);

    in->position  = &in->posbuf;		/* record position on same stream */
    out->position = &in->posbuf;
    err->position = &in->posbuf;

    in->encoding  = ENC_UTF8;
    out->encoding = ENC_UTF8;
    err->encoding = ENC_UTF8;

    ce->new_console(c, t2w(PL_A1));

    if (!PL_unify_stream(PL_A2, in) ||
        !PL_unify_stream(PL_A3, out) ||
        !PL_unify_stream(PL_A4, err)) {
            Sclose(in);
            Sclose(out);
            Sclose(err);
        return FALSE;
    }

    return TRUE;
}

/** append new command to history list for current console
 */
PREDICATE(rl_add_history, 1) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        WCP line = PL_A1;
        if (*line)
            c->add_history_line(QString::fromWCharArray(line));
        return TRUE;
    }
    return FALSE;
}

/** this should only be used as flag to enable processing ?
 */
PREDICATE(rl_read_init_file, 1) {
    Q_UNUSED(PL_A1);
    return TRUE;
}

/** get history lines for this console
 */
NAMED_PREDICATE("$rl_history", rl_history, 1) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        PlTail lines(PL_A1);
        foreach(QString x, c->history_lines())
            lines.append(W(x));
        lines.close();
        return TRUE;
    }
    return FALSE;
}

/** attempt to overcome default tty_size/2
 */
PREDICATE(tty_size, 2) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        QSize sz = c->fontMetrics().size(0, "Q");
        long Rows = c->height() / sz.height();
        long Cols = c->width() / sz.width();
        PL_A1 = Rows;
        PL_A2 = Cols;
        return TRUE;
    }
    return FALSE;
}

/** break looping
PREDICATE0(interrupt) {
    throw PlException(PlAtom("stop_req"));
    return FALSE;
}
*/

/** display modal message box
 *  win_message_box(+Text, +Options)
 *
 *  Options is list of name(Value). Currently only
 *   image - an image file name (can be resource based)
 *   title - the message box title
 *   icon  - identifier among predefined Qt message box icons
 *   image - pixmap file (ok resource)
 *   image_scale - multiplier to scale image
 */
PREDICATE(win_message_box, 2) {
    ConsoleEdit* c = console_by_thread();

    if (c) {
        QString Text = t2w(PL_A1);

        QString Title = "swipl-win", Image;
        PlTerm Icon; //QMessageBox::Icon Icon = QMessageBox::NoIcon;

        // scan options
        float scale = 0;
        PlTerm Option;
        int min_width = 0;

        for (PlTail t(PL_A2); t.next(Option); )
            if (Option.arity() == 1) {
                QString name = Option.name();
                if (name == "title")
                    Title = t2w(Option[1]);
                if (name == "icon")
                    Icon = Option[1];
                if (name == "image")
                    Image = t2w(Option[1]);
                if (name == "image_scale")
                    scale = double(Option[1]);
                if (name == "min_width")
                    min_width = int(Option[1]);
            }
            else
                throw PlException(A(c->tr("option %1 : invalid arity").arg(t2w(Option))));

        int rc;
        QString err;
        ConsoleEdit::exec_sync s;

        c->exec_func([&]() {

            QMessageBox mbox(c);

            // get icon file, if required
            QPixmap imfile;
            if (!Image.isEmpty()) {
                if (!imfile.load(Image)) {
                    err = c->tr("icon file %1 not found").arg(Image);
                    return;
                }
                if (scale)
                    imfile = imfile.scaled(imfile.size() * scale,
                                           Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }

            mbox.setText(Text);
            mbox.setWindowTitle(Title);
            if (!imfile.isNull())
                mbox.setIconPixmap(imfile);

            if (min_width) {
                auto horizontalSpacer = new QSpacerItem(min_width, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
                auto layout = qobject_cast<QGridLayout*>(mbox.layout());
                layout->addItem(horizontalSpacer, layout->rowCount(), 0, 1, layout->columnCount());
            }

            rc = mbox.exec() == mbox.Ok;
            s.go();
        });
        s.stop();

        if (!err.isEmpty())
            throw PlException(A(err));

        return rc;
    }

    return FALSE;
}

/** interrupt/0
 *  Ctrl+C
 */
PREDICATE0(interrupt) {
    ConsoleEdit* c = console_by_thread();
    qDebug() << "interrupt" << CVP(c);
    if (c) {
        c->int_request();
        return TRUE;
    }
    return FALSE;
}

#undef PROLOG_MODULE
#define PROLOG_MODULE "pqConsole"

/** set/get settings of thread associated console
 *  some selected property
 *
 *  updateRefreshRate(N) default 100
 *  - allow to alter default refresh rate (simply count outputs before setting cursor at end)
 *
 *  maximumBlockCount(N) default 0
 *  - remove (from top) text lines when exceeding the limit
 *
 *  lineWrapMode(Mode) Mode --> 'NoWrap' | 'WidgetWidth'
 *  - when NoWrap, an horizontal scroll bar could display
 */
PREDICATE(console_settings, 1) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        PlFrame fr;
        PlTerm opt;
        for (PlTail opts(PL_A1); opts.next(opt); ) {
            if (opt.arity() == 1)
                unify(opt.name(), c, opt[1]);
            else
                throw PlException(A(c->tr("%1: properties have arity 1").arg(t2w(opt))));
        }
        return TRUE;
    }
    return FALSE;
}

/** getOpenFileName(+Title, ?StartPath, +Pattern, -Choice)
 *  run a modal dialog on request from foreign thread
 *  this must run a modal loop in GUI thread
 */
PREDICATE(getOpenFileName, 4) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        QString Caption = t2w(PL_A1), StartPath, Pattern = t2w(PL_A3), Choice;
        if (PL_A2.type() == PL_ATOM)
            StartPath = t2w(PL_A2);

        ConsoleEdit::exec_sync s;

        c->exec_func([&]() {
            Choice = QFileDialog::getOpenFileName(c, Caption, StartPath, Pattern);
            s.go();
        });
        s.stop();

        if (!Choice.isEmpty()) {
            PL_A4 = A(Choice);
            return TRUE;
        }
    }
    return FALSE;
}

/** getSaveFileName(+Title, ?StartPath, +Pattern, -Choice)
 *  run a modal dialog on request from foreign thread
 *  this must run a modal loop in GUI thread
 */
PREDICATE(getSaveFileName, 4) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        QString Caption = t2w(PL_A1), StartPath, Pattern = t2w(PL_A3), Choice;
        if (PL_A2.type() == PL_ATOM)
            StartPath = t2w(PL_A2);

        ConsoleEdit::exec_sync s;
        c->exec_func([&]() {
            Choice = QFileDialog::getSaveFileName(c, Caption, StartPath, Pattern);
            s.go();
        });
        s.stop();

        if (!Choice.isEmpty()) {
            PL_A4 = A(Choice);
            return TRUE;
        }
    }
    return FALSE;
}

/** select_font
 *  run Qt font selection
 */
PREDICATE0(select_font) {
    ConsoleEdit* c = console_by_thread();
    bool ok = false;
    if (c) {
        ConsoleEdit::exec_sync s;
        c->exec_func([&]() {
            Preferences p;
            QFont font = QFontDialog::getFont(&ok, p.console_font, c);
            if (ok)
                c->setFont(p.console_font = font);
            s.go();
        });
        s.stop();
    }
    return ok;
}

/** select_ANSI_term_colors
 *  run a dialog to let user configure console colors (associate user defined color to indexes 1-16)
 */
PREDICATE0(select_ANSI_term_colors) {
    ConsoleEdit* c = console_by_thread();
    bool ok = false;
    if (c) {
        ConsoleEdit::exec_sync s;
        c->exec_func([&]() {
            Preferences p;
            QColorDialog d(c);
	    d.setOption(QColorDialog::ColorDialogOption::DontUseNativeDialog);
            Q_ASSERT(d.customCount() >= p.ANSI_sequences.size());
            for (int i = 0; i < p.ANSI_sequences.size(); ++i)
                d.setCustomColor(i, p.ANSI_sequences[i].rgb());
            if (d.exec()) {
                for (int i = 0; i < p.ANSI_sequences.size(); ++i)
                    p.ANSI_sequences[i] = d.customColor(i);
                c->repaint();
                ok = true;
            }
            s.go();
        });
        s.stop();
        return ok;
    }
    return FALSE;
}

/** quit_console
 *  just issue termination to Qt application object
 */
PREDICATE0(quit_console) {

    ConsoleEdit* c = console_by_thread();
    if (c) {
        // run on foreground
        c->exec_func([]() { QApplication::postEvent(qApp, new QCloseEvent); });
        return TRUE;
    }

    return FALSE;
}

/** issue a copy to clipboard of current selection
 */
PREDICATE0(copy) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        c->exec_func([=](){
            QApplication::clipboard()->setText(c->textCursor().selectedText());
            do_events();
        });
        return TRUE;
    }
    return FALSE;
}

/** issue a paste to clipboard of current selection
 */
PREDICATE0(paste) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        c->exec_func([=](){
            c->textCursor().insertText(QApplication::clipboard()->text());
            do_events();
        });
        return TRUE;
    }
    return FALSE;
}

#undef PROLOG_MODULE
#define PROLOG_MODULE "system"

/** win_preference_groups(-Groups:list)
 */
PREDICATE(win_preference_groups, 1) {
    Preferences p;
    PlTail l(PL_A1);
    foreach (auto g, p.childGroups())
        l.append(A(g));
    l.close();
    return TRUE;
}

/** win_preference_keys(+Group, -Keys:list)
 */
PREDICATE(win_preference_keys, 2) {
    Preferences p;
    PlTail l(PL_A1);
    foreach (auto k, p.childKeys())
        l.append(A(k));
    l.close();
    return TRUE;
}

/** win_current_preference(+Group, +Key, -Value)
 */
PREDICATE(win_current_preference, 3) {
    Preferences p;

    auto g = t2w(PL_A1),
         k = t2w(PL_A2);

    p.beginGroup(g);
    if (p.contains(k)) {
        auto x = p.value(k).toString();
        return PL_A3 = PlCompound(x.toStdWString().data());
    }

    return FALSE;
}

/** win_set_preference(+Group, +Key, +Value)
 */
PREDICATE(win_set_preference, 3) {
    Preferences p;

    auto g = t2w(PL_A1),
         k = t2w(PL_A2);

    p.beginGroup(g);
    p.setValue(k, serialize(PL_A3));
    return TRUE;
}

/** output html at prompt
 */
PREDICATE(win_html_write, 1) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        // run on foreground
        QString html = t2w(PL_A1);
        ConsoleEdit::exec_sync s;
        c->exec_func([&]() {
            c->html_write(html);
            s.go();
        });
        s.stop();
        return TRUE;
    }
    return FALSE;
}
