#include "ui/settings_dialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace dv::ui {
namespace {

/// Section 6 of SPEC.md suggests 1.5 to 3 Mbps. The range offered around it is
/// wide enough to be useful on a bad link and on a local network, and narrow
/// enough that nobody sets something absurd by dragging.
constexpr int kMinBitrateKbps = 200;
constexpr int kMaxBitrateKbps = 8000;
constexpr int kBitrateStepKbps = 100;

}  // namespace

SettingsDialog::SettingsDialog(client::app::CallSession& session, QWidget* parent)
    : QDialog(parent), session_(session) {
  setWindowTitle(QStringLiteral("Configurações"));
  setMinimumWidth(520);

  auto* layout = new QVBoxLayout(this);

  auto* audio = new QGroupBox(QStringLiteral("Áudio"), this);
  auto* audio_form = new QFormLayout(audio);
  input_ = new QComboBox(audio);
  output_ = new QComboBox(audio);
  audio_form->addRow(QStringLiteral("Microfone"), input_);
  audio_form->addRow(QStringLiteral("Saída"), output_);

  auto* video = new QGroupBox(QStringLiteral("Tela"), this);
  auto* video_form = new QFormLayout(video);
  monitor_ = new QComboBox(video);

  min_bitrate_ = new QSpinBox(video);
  min_bitrate_->setRange(kMinBitrateKbps, kMaxBitrateKbps);
  min_bitrate_->setSingleStep(kBitrateStepKbps);
  min_bitrate_->setSuffix(QStringLiteral(" kbps"));

  max_bitrate_ = new QSpinBox(video);
  max_bitrate_->setRange(kMinBitrateKbps, kMaxBitrateKbps);
  max_bitrate_->setSingleStep(kBitrateStepKbps);
  max_bitrate_->setSuffix(QStringLiteral(" kbps"));

  video_form->addRow(QStringLiteral("Monitor"), monitor_);
  video_form->addRow(QStringLiteral("Bitrate mínimo"), min_bitrate_);
  video_form->addRow(QStringLiteral("Bitrate máximo"), max_bitrate_);

  auto* note =
      new QLabel(QStringLiteral("As mudanças valem na hora, inclusive durante uma chamada."), this);
  note->setStyleSheet(QStringLiteral("color: palette(mid);"));

  // Named here rather than taken from QDialogButtonBox::Close, whose label
  // comes from Qt's own translations. Without a translation file loaded that
  // is the English word, sitting in the middle of an interface that is not.
  auto* buttons = new QDialogButtonBox(this);
  buttons->addButton(QStringLiteral("Fechar"), QDialogButtonBox::AcceptRole);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

  layout->addWidget(audio);
  layout->addWidget(video);
  layout->addWidget(note);
  layout->addWidget(buttons);

  load_devices();
  load_monitors();

  const auto [minimum, maximum] = session_.video_bitrate();
  min_bitrate_->setValue(minimum);
  max_bitrate_->setValue(maximum);

  // Connected after the initial values are in, so that filling the widgets
  // does not look like the user changing something.
  connect(input_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_input_changed);
  connect(output_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_output_changed);
  connect(min_bitrate_, &QSpinBox::editingFinished, this, &SettingsDialog::on_bitrate_changed);
  connect(max_bitrate_, &QSpinBox::editingFinished, this, &SettingsDialog::on_bitrate_changed);
}

void SettingsDialog::load_devices() {
  const auto inputs = client::media::input_devices();
  if (inputs) {
    for (const client::media::AudioDevice& device : inputs.value()) {
      input_->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
    }
  }
  if (input_->count() == 0) {
    input_->addItem(QStringLiteral("nenhum microfone disponível"), QString{});
    input_->setEnabled(false);
  }

  const auto outputs = client::media::output_devices();
  if (outputs) {
    for (const client::media::AudioDevice& device : outputs.value()) {
      output_->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
    }
  }
  if (output_->count() == 0) {
    output_->addItem(QStringLiteral("nenhuma saída disponível"), QString{});
    output_->setEnabled(false);
  }
}

void SettingsDialog::load_monitors() {
  const auto listed = session_.monitors();
  if (listed) {
    for (const client::video::Monitor& screen : listed.value()) {
      monitor_->addItem(QString::fromStdString(screen.name), QString::fromStdString(screen.id));
    }
  }
  if (monitor_->count() == 0) {
    monitor_->addItem(QStringLiteral("nenhum monitor disponível"), QString{});
    monitor_->setEnabled(false);
  }
}

void SettingsDialog::on_input_changed(int index) {
  if (index < 0) {
    return;
  }
  (void)session_.set_input_device(input_->itemData(index).toString().toStdString());
}

void SettingsDialog::on_output_changed(int index) {
  if (index < 0) {
    return;
  }
  (void)session_.set_output_device(output_->itemData(index).toString().toStdString());
}

void SettingsDialog::on_bitrate_changed() {
  // The maximum cannot sit below the minimum, and rather than refusing the
  // edit the other end is moved to keep it sensible.
  if (max_bitrate_->value() < min_bitrate_->value()) {
    max_bitrate_->setValue(min_bitrate_->value());
  }
  (void)session_.set_video_bitrate(min_bitrate_->value(), max_bitrate_->value());
}

QString SettingsDialog::selected_monitor() const {
  return monitor_->currentData().toString();
}

}  // namespace dv::ui
