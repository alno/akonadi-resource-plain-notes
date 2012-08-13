#include "plainnotesresource.h"

#include "settings.h"
#include "settingsadaptor.h"
#include "settingsdialog.h"

#include <QtDBus/QDBusConnection>

#include <KLocale>
#include <KMime/KMimeMessage>

using namespace Akonadi;

PlainNotesResource::PlainNotesResource( const QString &id )
  : ResourceBase( id ),
  mSettings( new PlainNotesResourceSettings() )
{  
  new PlainNotesResourceSettingsAdaptor( mSettings );
  QDBusConnection::sessionBus().registerObject( QLatin1String( "/Settings" ), mSettings, QDBusConnection::ExportAdaptors );

  mItemMimeType = QLatin1String( "text/x-vnd.akonadi.note" );
  mSupportedMimeTypes << Collection::mimeType() << mItemMimeType;
  
  initializeDirectory( baseDirectoryPath() );
}

PlainNotesResource::~PlainNotesResource()
{
}

void PlainNotesResource::retrieveCollections()
{
  // create the resource collection
  Collection resourceCollection;
  resourceCollection.setParentCollection( Collection::root() );
  resourceCollection.setRemoteId( baseDirectoryPath() );
  resourceCollection.setName( name() );
  resourceCollection.setContentMimeTypes( mSupportedMimeTypes );
  resourceCollection.setRights( supportedRights( true ) );

  const QDir baseDir( baseDirectoryPath() );

  Collection::List collections = createCollectionsForDirectory( baseDir, resourceCollection );
  collections.append( resourceCollection );

  collectionsRetrieved( collections );
}

void PlainNotesResource::retrieveItems( const Akonadi::Collection &collection )
{
  QDir directory( directoryForCollection( collection ) );
  if ( !directory.exists() ) {
    cancelTask( i18n( "Directory '%1' does not exists", collection.remoteId() ) );
    return;
  }

  directory.setFilter( QDir::Files | QDir::Readable );

  Item::List items;

  const QFileInfoList entries = directory.entryInfoList();

  foreach ( const QFileInfo &entry, entries ) {
    if ( entry.fileName() == "WARNING_README.txt" )
      continue;

    Item item;
    item.setRemoteId( entry.fileName() );
    item.setMimeType( mItemMimeType );

    items.append( item );
  }

  itemsRetrieved( items );
}

bool PlainNotesResource::retrieveItem( const Akonadi::Item &item, const QSet<QByteArray> &parts )
{
  Q_UNUSED( parts );
  
  const QString filePath = directoryForCollection( item.parentCollection() ) + QDir::separator() + item.remoteId();

  QFile file( filePath );
  if ( !file.open( QIODevice::ReadOnly ) ) {
    cancelTask( i18n( "Unable to open file '%1'", filePath ) );
    return false;
  }

  KMime::Message::Ptr msg( new KMime::Message() );  
  msg->setHead( item.remoteId().toLocal8Bit() );
  msg->setContent( file.readAll() );

  file.close();
    
  Item newItem( item );
  newItem.setPayload( msg );
  itemRetrieved( newItem );

  return true;
}

void PlainNotesResource::aboutToQuit()
{
  mSettings->writeConfig();
}

void PlainNotesResource::configure( WId windowId )
{
  SettingsDialog dlg( mSettings, windowId );
  
  if ( dlg.exec() ) {
    mSettings->writeConfig();

    clearCache();
    initializeDirectory( baseDirectoryPath() );

    synchronize();

    emit configurationDialogAccepted();
  } else {
    emit configurationDialogRejected();
  }
}

void PlainNotesResource::itemAdded( const Akonadi::Item &item, const Akonadi::Collection &collection )
{
  saveItem( item, collection, true, true );
}

void PlainNotesResource::itemChanged( const Akonadi::Item &item, const QSet<QByteArray> &parts )
{
  bool bodyChanged = false;
  bool headChanged = false;
  
  foreach ( const QByteArray &part, parts )  {
    if ( part.startsWith( "PLD:RFC822" ) ) {
      bodyChanged = true;
    } else if ( part.startsWith( "PLD:HEAD" ) ) {
      headChanged = true;
    }
  }

  saveItem( item, item.parentCollection(), bodyChanged, headChanged );  
}

void PlainNotesResource::saveItem( const Akonadi::Item &item, const Akonadi::Collection &parentCollection, bool saveHead, bool saveBody )
{  
  if ( !saveHead && !saveBody ) {
    changeProcessed();
    return;
  }
  
  if ( mSettings->readOnly() ) {
    cancelTask( i18n( "Trying to write to a read-only file: '%1'", item.remoteId() ) );
    return;
  }
  
  Item newItem( item );
  
  if ( item.hasPayload<KMime::Message::Ptr>() ) { //something has changed that we can deal with
    const KMime::Message::Ptr mail = item.payload<KMime::Message::Ptr>();
    
    if ( saveHead ) {
      const QString sourceFilePath = directoryForCollection( parentCollection ) + QDir::separator() + item.remoteId();      
      const QString destinationFilePath = directoryForCollection( parentCollection ) + QDir::separator() + mail->head();
      
      QFile::rename(sourceFilePath, destinationFilePath);
      
      newItem.setRemoteId( mail->head() );
    }
    
    if ( saveBody ) {
      const QString filePath = directoryForCollection( item.parentCollection() ) + QDir::separator() + item.remoteId();
      QFile file( filePath );
      
      if ( !file.open( QIODevice::WriteOnly ) ) {
        cancelTask( i18n( "Unable to write to file '%1': %2", filePath, file.errorString() ) );
        return;
      }

      file.write( mail->decodedContent() );
      file.close();
    }
  } else {
    kWarning() << "got item without (usable) payload, ignoring it";
  }
  
  changeCommitted( newItem );
}

void PlainNotesResource::itemRemoved( const Akonadi::Item &item )
{
  if ( mSettings->readOnly() ) {
    cancelTask( i18n( "Trying to write to a read-only file: '%1'", item.remoteId() ) );
    return;
  }

  // If the parent collection has no valid remote id, the parent
  // collection will be removed in a second, so stop here and remove
  // all items in collectionRemoved().
  if ( item.parentCollection().remoteId().isEmpty() ) {
    changeProcessed();
    return;
  }

  const QString fileName = directoryForCollection( item.parentCollection() ) + QDir::separator() + item.remoteId();

  if ( !QFile::remove( fileName ) ) {
    cancelTask( i18n( "Unable to remove file '%1'", fileName ) );
    return;
  }

  changeProcessed();
}

void PlainNotesResource::itemMoved( const Akonadi::Item &item, const Akonadi::Collection &collectionSource,
                                  const Akonadi::Collection &collectionDestination )
{
  const QString sourceFileName = directoryForCollection( collectionSource ) + QDir::separator() + item.remoteId();
  const QString targetFileName = directoryForCollection( collectionDestination ) + QDir::separator() + item.remoteId();

  if ( QFile::rename( sourceFileName, targetFileName ) )
    changeProcessed();
  else
    cancelTask( i18n( "Unable to move file '%1' to '%2', '%2' already exists.", sourceFileName, targetFileName ) );
}

// Collection handling

void PlainNotesResource::collectionAdded( const Akonadi::Collection &collection, const Akonadi::Collection &parent )
{
  if ( mSettings->readOnly() ) {
    cancelTask( i18n( "Trying to write to a read-only directory: '%1'", parent.remoteId() ) );
    return;
  }

  const QString dirName = directoryForCollection( parent ) + QDir::separator() + collection.name();

  if ( !QDir::root().mkpath( dirName ) ) {
    cancelTask( i18n( "Unable to create folder '%1'.", dirName ) );
    return;
  }

  initializeDirectory( dirName );

  Collection newCollection( collection );
  newCollection.setRemoteId( collection.name() );
  changeCommitted( newCollection );
}

void PlainNotesResource::collectionChanged( const Akonadi::Collection &collection )
{
  if ( mSettings->readOnly() ) {
    cancelTask( i18n( "Trying to write to a read-only directory: '%1'", collection.remoteId() ) );
    return;
  }

  if ( collection.parentCollection() == Collection::root() ) {
    if ( collection.name() != name() )
      setName( collection.name() );
    changeProcessed();
    return;
  }

  if ( collection.remoteId() == collection.name() ) {
    changeProcessed();
    return;
  }

  const QString dirName = directoryForCollection( collection );

  QFileInfo oldDirectory( dirName );
  if ( !QDir::root().rename( dirName, oldDirectory.absolutePath() + QDir::separator() + collection.name() ) ) {
    cancelTask( i18n( "Unable to rename folder '%1'.", collection.name() ) );
    return;
  }

  Collection newCollection( collection );
  newCollection.setRemoteId( collection.name() );
  changeCommitted( newCollection );
}

void PlainNotesResource::collectionRemoved( const Akonadi::Collection &collection )
{
  if ( mSettings->readOnly() ) {
    cancelTask( i18n( "Trying to write to a read-only directory: '%1'", collection.remoteId() ) );
    return;
  }

  if ( !removeDirectory( directoryForCollection( collection ) ) ) {
    cancelTask( i18n( "Unable to delete folder '%1'.", collection.name() ) );
    return;
  }

  changeProcessed();
}

void PlainNotesResource::collectionMoved( const Akonadi::Collection &collection, const Akonadi::Collection &collectionSource,
                                        const Akonadi::Collection &collectionDestination )
{
  const QString sourceDirectoryName = directoryForCollection( collectionSource ) + QDir::separator() + collection.remoteId();
  const QString targetDirectoryName = directoryForCollection( collectionDestination ) + QDir::separator() + collection.remoteId();

  if ( QFile::rename( sourceDirectoryName, targetDirectoryName ) )
    changeProcessed();
  else
    cancelTask( i18n( "Unable to move directory '%1' to '%2', '%2' already exists.", sourceDirectoryName, targetDirectoryName ) );
}

// Internal helpers

QString PlainNotesResource::baseDirectoryPath() const
{
  return mSettings->path();
}

bool PlainNotesResource::removeDirectory( const QDir &directory )
{
  const QFileInfoList infos = directory.entryInfoList( QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot );
  foreach ( const QFileInfo &info, infos ) {
    if ( info.isDir() ) {
      if ( !removeDirectory( QDir( info.absoluteFilePath() ) ) )
        return false;
    } else {
      if ( !QFile::remove( info.filePath() ) )
        return false;
    }
  }

  if ( !QDir::root().rmdir( directory.absolutePath() ) )
    return false;

  return true;
}

void PlainNotesResource::initializeDirectory( const QString &path ) const
{
  QDir dir( path );

  // if folder does not exists, create it
  if ( !dir.exists() )
    QDir::root().mkpath( dir.absolutePath() );

  // check whether warning file is in place...
  QFile file( dir.absolutePath() + QDir::separator() + "WARNING_README.txt" );
  if ( !file.exists() ) {
    // ... if not, create it
    file.open( QIODevice::WriteOnly );
    file.write( "Important Warning!!!\n\n"
                "Don't create or copy vCards inside this folder manually, they are managed by the Akonadi framework!\n" );
    file.close();
  }
}

Collection::Rights PlainNotesResource::supportedRights( bool isResourceCollection ) const
{
  Collection::Rights rights = Collection::ReadOnly;

  if ( !mSettings->readOnly() ) {
    rights |= Collection::CanChangeItem;
    rights |= Collection::CanCreateItem;
    rights |= Collection::CanDeleteItem;
    rights |= Collection::CanCreateCollection;
    rights |= Collection::CanChangeCollection;

    if ( !isResourceCollection )
      rights |= Collection::CanDeleteCollection;
  }

  return rights;
}

QString PlainNotesResource::directoryForCollection( const Collection& collection ) const
{
  if ( collection.remoteId().isEmpty() ) {
    kWarning() << "Got incomplete ancestor chain:" << collection;
    return QString();
  }

  if ( collection.parentCollection() == Collection::root() ) {
    kWarning( collection.remoteId() != baseDirectoryPath() ) << "RID mismatch, is " << collection.remoteId()
                                                             << " expected " << baseDirectoryPath();
    return collection.remoteId();
  }

  const QString parentDirectory = directoryForCollection( collection.parentCollection() );
  if ( parentDirectory.isNull() ) // invalid, != isEmpty() here!
    return QString();

  QString directory = parentDirectory;
  if ( !directory.endsWith( '/' ) )
    directory += QDir::separator() + collection.remoteId();
  else
    directory += collection.remoteId();

  return directory;
}

Collection::List PlainNotesResource::createCollectionsForDirectory( const QDir &parentDirectory, const Collection &parentCollection ) const
{
  Collection::List collections;

  QDir dir( parentDirectory );
  dir.setFilter( QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable );
  const QFileInfoList entries = dir.entryInfoList();

  foreach ( const QFileInfo &entry, entries ) {
    QDir subdir( entry.absoluteFilePath() );

    Collection collection;
    collection.setParentCollection( parentCollection );
    collection.setRemoteId( entry.fileName() );
    collection.setName( entry.fileName() );
    collection.setContentMimeTypes( mSupportedMimeTypes );
    collection.setRights( supportedRights( false ) );

    collections << collection;
    collections << createCollectionsForDirectory( subdir, collection );
  }

  return collections;
}

AKONADI_RESOURCE_MAIN( PlainNotesResource )

#include "plainnotesresource.moc"
