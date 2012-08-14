#include "plainnotesresource.h"

#include "settings.h"
#include "settingsadaptor.h"
#include "settingsdialog.h"

#include <QtDBus/QDBusConnection>

#include <Akonadi/ChangeRecorder>
#include <Akonadi/ItemFetchScope>
#include <Akonadi/ItemFetchJob>
#include <Akonadi/ItemModifyJob>
#include <Akonadi/CollectionFetchScope>
#include <Akonadi/CollectionFetchJob>

#include <KLocale>
#include <KDirWatch>
#include <KMime/KMimeMessage>

#define ENCODING "utf-8"
#define X_NOTES_LASTMODIFIED_HEADER "X-Akonotes-LastModified"

using namespace Akonadi;

PlainNotesResource::PlainNotesResource( const QString &id )
  : ResourceBase( id ),
  mSettings( new PlainNotesResourceSettings() ),
  mFsWatcher( new KDirWatch( this ) )
{
  new PlainNotesResourceSettingsAdaptor( mSettings );
  QDBusConnection::sessionBus().registerObject( QLatin1String( "/Settings" ), mSettings, QDBusConnection::ExportAdaptors );

  changeRecorder()->fetchCollection( true );
  changeRecorder()->itemFetchScope().fetchFullPayload( true );
  changeRecorder()->itemFetchScope().setAncestorRetrieval( ItemFetchScope::All ); // Retrieve all item ancestors for correct file name selection
  changeRecorder()->collectionFetchScope().setAncestorRetrieval( CollectionFetchScope::All ); // Retrieve all collection ancestors for correct file name selection

  setHierarchicalRemoteIdentifiersEnabled( true );

  mItemMimeType = QLatin1String( "text/x-vnd.akonadi.note" );
  mSupportedMimeTypes << Collection::mimeType() << mItemMimeType;

  initializeDirectory( baseDirectoryPath() );

  connect( mFsWatcher, SIGNAL(dirty(QString)), SLOT(directoryChanged(QString)) );

  synchronizeCollectionTree();
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
    if ( isIgnored( entry.fileName() ) )
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

  QString data = QTextStream( &file ).readAll();

  file.close();

  Item newItem( item );
  newItem.setMimeType( mItemMimeType );

  setItemPayload( newItem, filePath, data );
  itemRetrieved( newItem );

  return true;
}

void PlainNotesResource::setItemPayload( Akonadi::Item & item, QString file, QString data ) {
  QFileInfo fi( file );

  KMime::Message * msg = new KMime::Message();
  msg->subject( true )->fromUnicodeString( item.remoteId(), ENCODING );
  msg->contentType( true )->setMimeType( "text/plain" );
  msg->contentType( true )->setCharset( ENCODING );
  msg->date( true )->setDateTime( KDateTime( fi.created() ) );
  msg->mainBodyPart()->fromUnicodeString( data );
  msg->mainBodyPart()->changeEncoding( KMime::Headers::CEquPr );
  msg->appendHeader( new KMime::Headers::Generic( X_NOTES_LASTMODIFIED_HEADER, msg, KDateTime( fi.lastModified() ).toString( KDateTime::RFCDateDay ).toLatin1(), ENCODING ) );
  msg->assemble();

  item.setPayload( KMime::Message::Ptr( msg ) );
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

    kWarning() << "configured, watching" << endl;

    configurationDialogAccepted();
  } else {
    configurationDialogRejected();
  }
}

// Fs watching

void PlainNotesResource::directoryChanged( const QString &dir )
{
  QFileInfo fi( dir );

  if ( isIgnored( fi.fileName() ) ) {
    kDebug() << "Ignoring filtered out file/directory" << dir;
    return;
  }

  if ( fi.isFile() ) {
    fileChanged( dir );
    return;
  }

  kWarning() << "directory changed" << dir;

  if ( dir == baseDirectoryPath() ) {
    synchronize();
    return;
  }

  const Collection col = collectionForDirectory( dir );
  if ( col.remoteId().isEmpty() ) {
    kWarning() << "Unable to find collection for path" << dir;
    return;
  }

  CollectionFetchJob *job = new CollectionFetchJob( col, CollectionFetchJob::Base, this );
  connect( job, SIGNAL(result(KJob*)), SLOT(fsWatchDirFetchResult(KJob*)) );
}

void PlainNotesResource::fsWatchDirFetchResult(KJob* job)
{
  if ( job->error() ) {
    kDebug() << job->errorString();
    return;
  }

  const Collection::List cols = qobject_cast<CollectionFetchJob*>( job )->collections();

  if ( cols.isEmpty() )
    return;

  synchronizeCollection( cols.first().id() );
}

void PlainNotesResource::fileChanged( const QString &file )
{
  kWarning() << "file changed" << file;

  QFileInfo fi( file );

  QString key = fi.fileName();
  QString dir = fi.dir().path();

  const Collection col = collectionForDirectory( dir );
  if ( col.remoteId().isEmpty() ) {
    kDebug() << "Unable to find collection for path" << dir;
    return;
  }

  Item item;
  item.setRemoteId( key );
  item.setParentCollection( col );

  ItemFetchJob *job = new ItemFetchJob( item, this );
  job->fetchScope().setAncestorRetrieval( ItemFetchScope::All );
  connect( job, SIGNAL(result(KJob*)), SLOT(fsWatchFileFetchResult(KJob*)) );
}

void PlainNotesResource::fsWatchFileFetchResult( KJob* job )
{
  if ( job->error() ) {
    kDebug() << job->errorString();
    return;
  }

  Item::List items = qobject_cast<ItemFetchJob*>( job )->items();

  if ( items.isEmpty() )
    return;

  Item newItem( items.at( 0 ) );

  const QString filePath = directoryForCollection( newItem.parentCollection() ) + QDir::separator() + newItem.remoteId();

  QFile file( filePath );

  if ( !file.open( QIODevice::ReadOnly ) ) {
    kWarning() << "Unable to open file" << filePath ;
    return;
  }

  QString data = QTextStream( &file ).readAll();

  file.close();

  setItemPayload( newItem, filePath, data );

  new ItemModifyJob( newItem );
}

// Item handling

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

  saveItem( item, item.parentCollection(), headChanged, bodyChanged );
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

    if ( saveHead || newItem.remoteId().isEmpty() ) { // We should set remote id if it's empty or should be saved
      newItem.setRemoteId( mail->subject( true )->asUnicodeString() );

      if ( newItem.remoteId().isEmpty() ) { // If id is empty after we set it
        cancelTask( i18n( "Unable to set empty id from '%1'", newItem.remoteId() ) );
        return;
      }
    }

    if ( saveHead && !item.remoteId().isEmpty() && item.remoteId() != newItem.remoteId() ) { // We should rename old file it old id was not null
      const QString sourceFilePath = directoryForCollection( parentCollection ) + QDir::separator() + item.remoteId();
      const QString destinationFilePath = directoryForCollection( parentCollection ) + QDir::separator() + newItem.remoteId();

      if ( QFile::exists( sourceFilePath ) && !QFile::rename(sourceFilePath, destinationFilePath) ) { // If file exists but can't be renamed - it's a problem
        cancelTask( i18n( "Unable to rename file from '%1' to '%2'", sourceFilePath, destinationFilePath ) );
        return;
      }
    }

    if ( saveBody ) {
      const QString dirPath = directoryForCollection( parentCollection );
      const QString filePath = dirPath + QDir::separator() + newItem.remoteId();

      mFsWatcher->removeDir( dirPath );

      QFile file( filePath );
      QTextStream stream( &file );

      if ( !file.open( QIODevice::WriteOnly ) ) {
        cancelTask( i18n( "Unable to write to file '%1': %2", filePath, file.errorString() ) );
        return;
      }

      stream << mail->mainBodyPart()->decodedText( true, true );
      stream.flush();

      file.close();

      mFsWatcher->addDir( dirPath, KDirWatch::WatchFiles );
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

  Collection newCollection( collection );
  newCollection.setRemoteId( collection.name() );

  const QString oldName = directoryForCollection( collection );
  const QString newName = directoryForCollection( newCollection );

  if ( !QFile::rename( oldName, newName ) ) {
    cancelTask( i18n( "Unable to rename folder '%1' from '%2' to '%3'.", collection.name(), oldName, newName ) );
    return;
  }

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
  return QDir::cleanPath( mSettings->path() );
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

  if ( !dir.exists() )
    QDir::root().mkpath( dir.absolutePath() );
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

  return parentDirectory + QDir::separator() + collection.remoteId();
}

Collection PlainNotesResource::collectionForDirectory( const QString & path ) const
{
  QFileInfo fi( path );
  Collection col;

  if ( fi.filePath() == baseDirectoryPath() ) {
    col.setRemoteId( fi.filePath() );
    col.setParentCollection( Collection::root() );
  } else {
    col.setRemoteId( fi.fileName() );
    col.setParentCollection( collectionForDirectory( fi.path() ) );
  }

  return col;
}

Collection::List PlainNotesResource::createCollectionsForDirectory( const QDir &parentDirectory, const Collection &parentCollection ) const
{
  mFsWatcher->addDir( parentDirectory.path(), KDirWatch::WatchFiles );

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

bool PlainNotesResource::isIgnored( QString file ) const
{
  return file.startsWith(".") || file.startsWith("~") || file.endsWith("~");
}

AKONADI_RESOURCE_MAIN( PlainNotesResource )

#include "plainnotesresource.moc"
