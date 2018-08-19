#include "effect.h"

#include "panels/panels.h"
#include "panels/viewer.h"
#include "ui/viewerwidget.h"
#include "ui/collapsiblewidget.h"
#include "panels/project.h"
#include "project/undo.h"
#include "ui/labelslider.h"
#include "ui/colorbutton.h"
#include "ui/comboboxex.h"
#include "ui/fontcombobox.h"
#include "ui/checkboxex.h"
#include "project/clip.h"
#include "panels/timeline.h"

#include "effects/video/transformeffect.h"
#include "effects/video/inverteffect.h"
#include "effects/video/shakeeffect.h"
#include "effects/video/solideffect.h"
#include "effects/video/texteffect.h"

#include "effects/audio/paneffect.h"
#include "effects/audio/volumeeffect.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QTextEdit>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

QVector<QString> video_effect_names;
QVector<QString> audio_effect_names;

void init_effects() {
	video_effect_names.resize(VIDEO_EFFECT_COUNT);
	audio_effect_names.resize(AUDIO_EFFECT_COUNT);

	video_effect_names[VIDEO_TRANSFORM_EFFECT] = "Transform";
	video_effect_names[VIDEO_SHAKE_EFFECT] = "Shake";
	video_effect_names[VIDEO_TEXT_EFFECT] = "Text";
	video_effect_names[VIDEO_SOLID_EFFECT] = "Solid";
	video_effect_names[VIDEO_INVERT_EFFECT] = "Invert";

	audio_effect_names[AUDIO_VOLUME_EFFECT] = "Volume";
	audio_effect_names[AUDIO_PAN_EFFECT] = "Pan";
}

Effect* create_effect(int effect_id, Clip* c) {
	if (c->track < 0) {
		switch (effect_id) {
		case VIDEO_TRANSFORM_EFFECT: return new TransformEffect(c); break;
		case VIDEO_SHAKE_EFFECT: return new ShakeEffect(c); break;
		case VIDEO_TEXT_EFFECT: return new TextEffect(c); break;
		case VIDEO_SOLID_EFFECT: return new SolidEffect(c); break;
		case VIDEO_INVERT_EFFECT: return new InvertEffect(c); break;
		}
	} else {
		switch (effect_id) {
		case AUDIO_VOLUME_EFFECT: return new VolumeEffect(c); break;
		case AUDIO_PAN_EFFECT: return new PanEffect(c); break;
		}
	}
	qDebug() << "[ERROR] Invalid effect ID";
	return NULL;
}

Effect::Effect(Clip* c, int t, int i) :
	parent_clip(c),
	type(t),
	id(i),
	enable_image(false),
	enable_opengl(false)

{
    container = new CollapsibleWidget();
    if (type == EFFECT_TYPE_VIDEO) {
        container->setText(video_effect_names[i]);
    } else if (type == EFFECT_TYPE_AUDIO) {
        container->setText(audio_effect_names[i]);
	}
    connect(container->enabled_check, SIGNAL(clicked(bool)), this, SLOT(field_changed()));
    ui = new QWidget();

    ui_layout = new QGridLayout();
    ui->setLayout(ui_layout);
	container->setContents(ui);
}

Effect::~Effect() {
	for (int i=0;i<rows.size();i++) {
		delete rows.at(i);
	}
}

void Effect::copy_field_keyframes(Effect* e) {
	for (int i=0;i<rows.size();i++) {
		EffectRow* row = rows.at(i);
		for (int j=0;j<row->fieldCount();j++) {
			EffectField* field = row->field(j);
			e->rows.at(i)->field(j)->keyframes = field->keyframes;
		}
	}
}

EffectRow* Effect::add_row(const QString& name) {
	EffectRow* row = new EffectRow(this, ui_layout, name, rows.size());
	rows.append(row);
	return row;
}

EffectRow* Effect::row(int i) {
	return rows.at(i);
}

int Effect::row_count() {
	return rows.size();
}

void Effect::refresh() {}

void Effect::field_changed() {
	panel_viewer->viewer_widget->update();
}

bool Effect::is_enabled() {
    return container->enabled_check->isChecked();
}

void Effect::load(QXmlStreamReader* stream) {
	for (int i=0;i<rows.size();i++) {
		EffectRow* row = rows.at(i);
		while (!stream->atEnd() && !(stream->name() == "effect" && stream->isEndElement())) {
			stream->readNext();
			if (stream->name() == "row" && stream->isStartElement()) {
				for (int j=0;j<row->fieldCount();j++) {
					EffectField* field = row->field(j);
					while (!stream->atEnd() && !(stream->name() == "effect" && stream->isEndElement())) {
						stream->readNext();
						if (stream->name() == "field" && stream->isStartElement()) {
							stream->readNext();
							switch (field->type) {
							case EFFECT_FIELD_DOUBLE:
								field->set_double_value(stream->text().toDouble());
								break;
							case EFFECT_FIELD_COLOR:
								field->set_color_value(QColor(stream->text().toString()));
								break;
							case EFFECT_FIELD_STRING:
								field->set_string_value(stream->text().toString());
								break;
							case EFFECT_FIELD_BOOL:
								field->set_bool_value(stream->text() == "1");
								break;
							case EFFECT_FIELD_COMBO:
								field->set_combo_string(stream->text().toString());
								break;
							case EFFECT_FIELD_FONT:
								field->set_combo_string(stream->text().toString());
								break;
							}
							break;
						}
					}
				}
				break;
			}
		}
	}

}

void Effect::save(QXmlStreamWriter* stream) {
	for (int i=0;i<rows.size();i++) {
		EffectRow* row = rows.at(i);
		stream->writeStartElement("row");
		for (int j=0;j<row->fieldCount();j++) {
			EffectField* field = row->field(j);
			stream->writeStartElement("field");
			for (int k=0;k<field->keyframes.size();k++) {
				const EffectKeyframe& key = field->keyframes.at(i);
				stream->writeStartElement("key");
				stream->writeAttribute("frame", QString::number(key.frame));
				stream->writeAttribute("type", QString::number(key.type));
				switch (field->type) {
				case EFFECT_FIELD_DOUBLE: stream->writeAttribute("value", QString::number(key.data.toDouble())); break;
				case EFFECT_FIELD_COLOR: stream->writeAttribute("value", key.data.value<QColor>().name()); break;
				case EFFECT_FIELD_STRING: stream->writeAttribute("value", key.data.toString()); break;
				case EFFECT_FIELD_BOOL: stream->writeAttribute("value", QString::number(key.data.toBool())); break;
				case EFFECT_FIELD_COMBO: stream->writeAttribute("value", key.data.toString()); break;
				case EFFECT_FIELD_FONT: stream->writeAttribute("value", key.data.toString()); break;
				}
				stream->writeEndElement();
			}
			stream->writeEndElement(); // field
		}
		stream->writeEndElement(); // row
	}
}

Effect* Effect::copy(Clip*) {return NULL;}
void Effect::process_image(long, QImage&) {}
void Effect::process_gl(long, QOpenGLShaderProgram&, int*, int*) {}
void Effect::process_audio(uint8_t*, int) {}

/* Effect Row Definitions */

EffectRow::EffectRow(Effect *parent, QGridLayout *uilayout, const QString &n, int row) : parent_effect(parent), ui(uilayout), name(n), ui_row(row) {
	label = new QLabel(name);
	ui->addWidget(label, row, 0);

	/*keyframe_enable = new CheckboxEx();
	keyframe_enable->setToolTip("Enable Keyframes");
	ui->addWidget(keyframe_enable, row, 5);*/
}

EffectField* EffectRow::add_field(int type, int colspan) {
	EffectField* field = new EffectField(type);
	fields.append(field);
	QWidget* element = field->get_ui_element();
	ui->addWidget(element, ui_row, fields.size(), 1, colspan);
	return field;
}

EffectRow::~EffectRow() {
	for (int i=0;i<fields.size();i++) {
		delete fields.at(i);
	}
}

EffectField* EffectRow::field(int i) {
	return fields.at(i);
}

int EffectRow::fieldCount() {
	return fields.size();
}

/* Effect Field Definitions */

EffectField::EffectField(int t) : type(t) {
	// DEFAULT KEYFRAME
	EffectKeyframe key;
	key.frame = -1;
	keyframes.append(key);

	switch (t) {
	case EFFECT_FIELD_DOUBLE:
	{
		LabelSlider* ls = new LabelSlider();
		ui_element = ls;
		connect(ls, SIGNAL(valueChanged()), this, SIGNAL(changed()));
	}
		break;
	case EFFECT_FIELD_COLOR:
	{
		ColorButton* cb = new ColorButton();
		ui_element = cb;
		connect(cb, SIGNAL(color_changed()), this, SIGNAL(changed()));
	}
		break;
	case EFFECT_FIELD_STRING:
	{
		QTextEdit* edit = new QTextEdit();
		edit->setUndoRedoEnabled(true);
		ui_element = edit;
		connect(edit, SIGNAL(textChanged()), this, SIGNAL(changed()));
	}
		break;
	case EFFECT_FIELD_BOOL:
	{
		CheckboxEx* cb = new CheckboxEx();
		ui_element = cb;
		connect(cb, SIGNAL(clicked(bool)), this, SIGNAL(changed()));
		connect(cb, SIGNAL(toggled(bool)), this, SIGNAL(toggled(bool)));
	}
		break;
	case EFFECT_FIELD_COMBO:
	{
		ComboBoxEx* cb = new ComboBoxEx();
		ui_element = cb;
		connect(cb, SIGNAL(currentIndexChanged(int)), this, SIGNAL(changed()));
	}
		break;
	case EFFECT_FIELD_FONT:
	{
		FontCombobox* fcb = new FontCombobox();
		ui_element = fcb;
		connect(fcb, SIGNAL(currentIndexChanged(int)), this, SIGNAL(changed()));
	}
		break;
	}
}

bool EffectField::is_keyframed(long p) {
	return keyframes.size() == 0 || p < 0;
}

QVariant EffectField::get_keyframe_data(long p) {
	if (keyframes.size() == 1) {
		return keyframes.at(0).data;
	}

	EffectKeyframe* before = NULL;
	EffectKeyframe* after = NULL;
	for (int i=1;i<keyframes.size();i++) {
		if (keyframes.at(i).frame == p) {
			return keyframes.at(i).data;
		} else if (keyframes.at(i).frame < p && !(before != NULL && keyframes.at(i).frame <= before->frame)) {
			before = &keyframes[i];
		} else if (keyframes.at(i).frame > p && !(after != NULL && keyframes.at(i).frame >= after->frame)) {
			after = &keyframes[i];
		}
	}

	// interpolate data
	if (before != NULL && after != NULL) {
		double progress = (p - before->frame)/(after->frame - before->frame);
		switch (type) {
		case EFFECT_FIELD_DOUBLE:
		{
			return lerp(before->data.toDouble(), after->data.toDouble(), progress);
		}
			break;
		case EFFECT_FIELD_COLOR:
		{
			QColor before_color = before->data.value<QColor>();
			QColor after_color = after->data.value<QColor>();
			QColor interval_color = QColor(
						lerp(before_color.red(), after_color.red(), progress),
						lerp(before_color.green(), after_color.green(), progress),
						lerp(before_color.blue(), after_color.blue(), progress)
					);
			return QVariant(interval_color);
		}
			break;
		case EFFECT_FIELD_STRING:
		case EFFECT_FIELD_BOOL:
		case EFFECT_FIELD_COMBO:
		case EFFECT_FIELD_FONT:
			return before->data;
			break;
		}
	}

	// return pre or post data
	if (before != NULL) return before->data;
	if (after != NULL) return after->data;
}

QWidget* EffectField::get_ui_element() {
	return ui_element;
}

void EffectField::set_enabled(bool e) {
	ui_element->setEnabled(e);
}

double EffectField::get_double_value(long p) {
	return get_keyframe_data(p).toDouble();
}

void EffectField::set_double_value(double v) {

	static_cast<LabelSlider*>(ui_element)->set_value(v, false);
}

void EffectField::set_double_default_value(double v) {
	static_cast<LabelSlider*>(ui_element)->set_default_value(v);
}

void EffectField::set_double_minimum_value(double v) {
	static_cast<LabelSlider*>(ui_element)->set_minimum_value(v);
}

void EffectField::set_double_maximum_value(double v) {
	static_cast<LabelSlider*>(ui_element)->set_maximum_value(v);
}

void EffectField::add_combo_item(const QString& name, const QVariant& data) {
	static_cast<ComboBoxEx*>(ui_element)->addItem(name, data);
}

int EffectField::get_combo_index(long p) {
	return static_cast<ComboBoxEx*>(ui_element)->currentIndex();
}

const QVariant EffectField::get_combo_data(long p) {
	return static_cast<ComboBoxEx*>(ui_element)->currentData();
}

const QString EffectField::get_combo_string(long p) {
	return static_cast<ComboBoxEx*>(ui_element)->itemText(get_keyframe_data(p).toInt());
}

void EffectField::set_combo_index(int index) {
	static_cast<ComboBoxEx*>(ui_element)->setCurrentIndexEx(index);
}

void EffectField::set_combo_string(const QString& s) {
	static_cast<ComboBoxEx*>(ui_element)->setCurrentTextEx(s);
}

bool EffectField::get_bool_value(long p) {
	return static_cast<QCheckBox*>(ui_element)->isChecked();
}

void EffectField::set_bool_value(bool b) {
	return static_cast<QCheckBox*>(ui_element)->setChecked(b);
}

const QString EffectField::get_string_value(long p) {
	return static_cast<QTextEdit*>(ui_element)->toPlainText();
}

void EffectField::set_string_value(const QString& s) {
	static_cast<QTextEdit*>(ui_element)->setText(s);
}

const QString EffectField::get_font_name(long p) {
	return static_cast<FontCombobox*>(ui_element)->currentText();
}

void EffectField::set_font_name(const QString& s) {
	static_cast<FontCombobox*>(ui_element)->setCurrentText(s);
}

QColor EffectField::get_color_value(long p) {
	return static_cast<ColorButton*>(ui_element)->get_color();
}

void EffectField::set_color_value(QColor color) {
	static_cast<ColorButton*>(ui_element)->set_color(color);
}