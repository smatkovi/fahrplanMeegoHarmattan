# Console probe for the Transitous backend, see transitous-probe.cpp.
# Built separately from the application so that it needs neither the user
# interface nor a running session on the device:
#
#   mad -t <target> qmake tools/transitous-probe.pro && mad make

QT += core network script
QT -= gui
CONFIG += console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = transitous-probe

# Out of source, so a build for the device and one for the desktop Qt of
# the SDK do not hand each other their moc output.
MOC_DIR = $$OUT_PWD/tmp-probe
OBJECTS_DIR = $$OUT_PWD/tmp-probe
INCLUDEPATH += $$PWD/../src $$OUT_PWD/tmp-probe

DEFINES += FAHRPLAN_VERSION=\\\"probe\\\" FAHRPLAN_SETTINGS_NAMESPACE=\\\"smurfy\\\"

HEADERS += \
    ../src/parser/parser_abstract.h \
    ../src/parser/parser_definitions.h \
    ../src/parser/parser_transitous.h

SOURCES += \
    transitous-probe.cpp \
    ../src/parser/parser_abstract.cpp \
    ../src/parser/parser_definitions.cpp \
    ../src/parser/parser_transitous.cpp

LIBS += -lz
