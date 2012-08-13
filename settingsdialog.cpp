/*
    Copyright (c) 2008 Volker Krause <vkrause@kde.org>

    This library is free software; you can redistribute it and/or modify it
    under the terms of the GNU Library General Public License as published by
    the Free Software Foundation; either version 2 of the License, or (at your
    option) any later version.

    This library is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
    License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to the
    Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301, USA.
*/

#include "settingsdialog.h"
#include "settings.h"

#include <KConfigDialogManager>
#include <KUrlRequester>
#include <KLineEdit>
#include <KWindowSystem>

#include <KLocale>

SettingsDialog::SettingsDialog( PlainNotesResourceSettings *settings, WId windowId ) :
    KDialog(),
    mSettings( settings )
{
  if ( windowId )
    KWindowSystem::setMainWindow( this, windowId );
  
  setButtons( Ok | Cancel );
  setCaption( i18n( "Select a plain notes folder" ) );
  
  ui.setupUi( mainWidget() );  
  ui.kcfg_Path->setMode( KFile::Directory | KFile::ExistingOnly );
  ui.kcfg_Path->setUrl( KUrl( mSettings->path() ) );

  connect( this, SIGNAL(okClicked()), SLOT(save()) );
  connect( ui.kcfg_Path, SIGNAL(textChanged(QString)), SLOT(validate()) );
  connect( ui.kcfg_ReadOnly, SIGNAL(toggled(bool)), SLOT(validate()) );
    
  mManager = new KConfigDialogManager( this, mSettings );
  mManager->updateWidgets();
  
  validate();
}

void SettingsDialog::validate()
{
  if ( ui.kcfg_Path->url().isEmpty() ) {
    ui.statusLabel->setText( i18n( "The selected path is empty." ) );
    enableButton( Ok, false );
    return;
  }
  
  QFileInfo f( ui.kcfg_Path->url().toLocalFile() );

  if ( f.exists() && f.isDir() ) {
    ui.statusLabel->setText( i18n( "The selected path is a valid directory." ) );
    
    if ( f.isWritable() ) {
      ui.kcfg_ReadOnly->setEnabled( true );
    } else {
      ui.kcfg_ReadOnly->setEnabled( false );
      ui.kcfg_ReadOnly->setChecked( true );
    }
    
    enableButton( Ok, true );
  } else {
    ui.statusLabel->setText( i18n( "The selected path does not exist." ) );
    
    enableButton( Ok, false );
  }  
}

void SettingsDialog::save()
{
  mManager->updateSettings();
  QString path = ui.kcfg_Path->url().isLocalFile() ? ui.kcfg_Path->url().toLocalFile() : ui.kcfg_Path->url().path();
  mSettings->setPath( path );
  mSettings->writeConfig();

  if ( ui.kcfg_Path->url().isLocalFile() ) {
    QDir d( path );
    if ( !d.exists() ) {
      d.mkpath( ui.kcfg_Path->url().toLocalFile() );
    }
  }
}

#include "settingsdialog.moc"
