/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2013 Joshua C. Klontz                                           *
 *                                                                           *
 * Licensed under the Apache License, Version 2.0 (the "License");           *
 * you may not use this file except in compliance with the License.          *
 * You may obtain a copy of the License at                                   *
 *                                                                           *
 *     http://www.apache.org/licenses/LICENSE-2.0                            *
 *                                                                           *
 * Unless required by applicable law or agreed to in writing, software       *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and       *
 * limitations under the License.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <QtCore>
#include <QtWidgets>
#include <lua.hpp>

#include "likely.h"

class Messenger : public QObject
{
    Q_OBJECT
    static Messenger *singleton;

    Messenger() : QObject(0)
    {
        likely_set_message_callback(Messenger::send);
    }

    void sendMessage(QString message, bool error)
    {
        if (error)
            message = "<font color=\"red\">"+message+"</font>";
        emit newMessage(message);
    }

public:
    static Messenger *get()
    {
        if (!singleton)
            singleton = new Messenger();
        return singleton;
    }

    static void send(const char *message, bool error)
    {
        get()->sendMessage(message, error);
    }

signals:
    void newMessage(QString);
};

Messenger *Messenger::singleton = NULL;

class SyntaxHighlighter : public QSyntaxHighlighter
{
    QRegularExpression comments, keywords, numbers, strings, variables;
    QTextCharFormat commentsFont, keywordsFont, numbersFont, stringsFont, variablesFont;
    QStringList excludedVariables; // Lua internals

public:
    SyntaxHighlighter(QTextDocument *parent)
        : QSyntaxHighlighter(parent)
    {
        comments.setPattern("--\\N*");
        keywords.setPattern("\\b(?:and|break|do|else|elseif|"
                            "end|false|goto|for|function|"
                            "if|in|local|nil|not|"
                            "or|repeat|return|then|true|"
                            "until|while)\\b");
        numbers.setPattern("(?:0x[\\da-fA-F]*\\.?[\\da-fA-F]+(?:[pP]-?\\d+)?|-?\\d*\\.?\\d+(?:[Ee][+-]?\\d+)?)");
        strings.setPattern("\"[^\"]*+\"");
        commentsFont.setForeground(Qt::darkGray);
        keywordsFont.setForeground(Qt::darkYellow);
        numbersFont.setFontWeight(QFont::Bold);
        numbersFont.setForeground(Qt::darkBlue);
        stringsFont.setForeground(Qt::darkGreen);
        variablesFont.setFontWeight(QFont::Bold);

        lua_State *L = luaL_newstate();
        luaL_openlibs(L);
        excludedVariables = getGlobals(L, QStringList());
        lua_close(L);
    }

    void updateDictionary(lua_State *L)
    {
        variables.setPattern("\\b(?:" + getGlobals(L, excludedVariables).join('|') + ")\\b");
        rehighlight();
    }

private:
    void highlightBlock(const QString &text)
    {
        highlightHelp(text, keywords, keywordsFont);
        highlightHelp(text, numbers, numbersFont);
        highlightHelp(text, variables, variablesFont);
        highlightHelp(text, strings, stringsFont);
        highlightHelp(text, comments, commentsFont);
    }

    void highlightHelp(const QString &text, const QRegularExpression &re, const QTextCharFormat &font)
    {
        if (re.pattern().isEmpty())
            return;
        QRegularExpressionMatch match = re.match(text);
        int index = match.capturedStart();
        while (index >= 0) {
            const int length = match.capturedLength();
            setFormat(index, match.capturedLength(), font);
            match = re.match(text, index + length);
            index = match.capturedStart();
        }
    }

    static QStringList getGlobals(lua_State *L, const QStringList &exclude)
    {
        QStringList globals;
        lua_getglobal(L, "_G");
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            const QString global = lua_tostring(L, -2);
            if (!exclude.contains(global))
                globals.append(global);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return globals;
    }
};

class Source : public QTextEdit
{
    Q_OBJECT
    QString sourceFileName, previousSource;
    QSettings settings;
    lua_State *L;
    SyntaxHighlighter *highlighter;

public:
    Source(QWidget *p = 0) : QTextEdit(p), L(NULL)
    {
        highlighter = new SyntaxHighlighter(document());
        connect(this, SIGNAL(textChanged()), this, SLOT(exec()));
        setAcceptRichText(false);
        QString source = settings.value("source").toString();
        if (source.isEmpty())
            source = "-- Control-click bold source code to interact\n"
                     "lenna = read(\"img/Lenna.tiff\")\n\n"
                     "-- Console output appears below\n"
                     "print(\"Width:\" .. lenna.columns)\n"
                     "print(\"Height:\" .. lenna.rows)\n";
        setText(source);
    }

    static lua_State *exec(const QString &source, bool *error)
    {
        lua_State *L = luaL_newstate();
        luaL_openlibs(L);
        luaL_requiref(L, "likely", luaopen_likely, 1);
        lua_pop(L, 1);
        *error = luaL_dostring(L, likely_standard_library()) ||
                 luaL_dostring(L, qPrintable(source));
        return L;
    }

public slots:
    void setSource(QAction *a)
    {
        if (a->text() == "Open...") {
            sourceFileName = QFileDialog::getOpenFileName(NULL, "Open Source File");
            if (!sourceFileName.isEmpty()) {
                QFile file(sourceFileName);
                file.open(QFile::ReadOnly | QFile::Text);
                setText(QString::fromLocal8Bit(file.readAll()));
                file.close();
            }
        } else {
            if (sourceFileName.isEmpty() || (a->text() == "Save As..."))
                sourceFileName = QFileDialog::getSaveFileName(NULL, "Save Source File");
            if (!sourceFileName.isEmpty()) {
                QFile file(sourceFileName);
                file.open(QFile::WriteOnly | QFile::Text);
                file.write(toPlainText().toLocal8Bit());
                file.close();
            }
        }
    }

    void exec()
    {
        // This check needed because syntax highlighting triggers a textChanged() signal
        const QString source = toPlainText();
        if (source == previousSource) return;
        else previousSource = source;

        emit recompiling();
        if (L) lua_close(L);
        bool error;
        L = exec(source, &error);
        if (error) {
            Messenger::send(lua_tostring(L, -1), true);
            lua_pop(L, 1);
        }
        settings.setValue("source", source);
        highlighter->updateDictionary(L);
    }

private:
    void mousePressEvent(QMouseEvent *e)
    {
        QTextEdit::mousePressEvent(e);
        QTextCursor tc = textCursor();
        tc.select(QTextCursor::WordUnderCursor);
        const QString clickedWord = tc.selectedText();
    }

signals:
    void recompiling();
};

class Console : public QTextEdit
{
    Q_OBJECT

public:
    Console(QWidget *parent = 0)
        : QTextEdit(parent)
    {
        setReadOnly(true);
    }
};

int main(int argc, char *argv[])
{
    for (int i=1; i<argc; i++) {
        QString source;
        if (QFileInfo(argv[i]).exists()) {
            QFile file(argv[i]);
            file.open(QFile::ReadOnly | QFile::Text);
            source = file.readAll();
            file.close();
            if (source.startsWith("#!"))
                source = source.mid(source.indexOf('\n')+1);
        } else {
            source = argv[i];
        }

        bool error;
        lua_close(Source::exec(source, &error));
    }

    if (argc > 1)
        return 0;

    QApplication::setApplicationName("Dream");
    QApplication::setOrganizationName("Likely");
    QApplication::setOrganizationDomain("liblikely.org");
    QApplication application(argc, argv);

    QMenu *fileMenu = new QMenu("File");
    QAction *openSource = new QAction("Open...", fileMenu);
    QAction *saveSource = new QAction("Save", fileMenu);
    QAction *saveSourceAs = new QAction("Save As...", fileMenu);
    openSource->setShortcut(QKeySequence("Ctrl+O"));
    saveSource->setShortcut(QKeySequence("Ctrl+S"));
    saveSourceAs->setShortcut(QKeySequence("Ctrl+Shift+S"));
    fileMenu->addAction(openSource);
    fileMenu->addAction(saveSource);
    fileMenu->addAction(saveSourceAs);

    Source *source = new Source();
    Console *console = new Console();
    QObject::connect(fileMenu, SIGNAL(triggered(QAction*)), source, SLOT(setSource(QAction*)));
    QObject::connect(source, SIGNAL(recompiling()), console, SLOT(clear()));
    QObject::connect(Messenger::get(), SIGNAL(newMessage(QString)), console, SLOT(append(QString)));

    QMenuBar *menuBar = new QMenuBar();
    menuBar->addMenu(fileMenu);

    QGridLayout *centralWidgetLayout = new QGridLayout();
    centralWidgetLayout->addWidget(source, 0, 0);
    centralWidgetLayout->addWidget(console, 1, 0);
    centralWidgetLayout->setRowStretch(0, 4);
    centralWidgetLayout->setRowStretch(1, 1);
    QWidget *centralWidget = new QWidget();
    centralWidget->setLayout(centralWidgetLayout);

    QMainWindow mainWindow;
    mainWindow.setCentralWidget(centralWidget);
    mainWindow.setMenuBar(menuBar);
    mainWindow.setWindowTitle("Likely Dream");
    mainWindow.resize(800,600);
    mainWindow.show();

    return application.exec();
}

#include "dream.moc"
