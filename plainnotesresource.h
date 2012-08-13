#ifndef PLAINNOTESRESOURCE_H
#define PLAINNOTESRESOURCE_H

#include <Akonadi/ResourceBase>
#include <Akonadi/Collection>

#include <QDir>

class PlainNotesResourceSettings;

class PlainNotesResource : public Akonadi::ResourceBase,
                           public Akonadi::AgentBase::ObserverV2
{
  Q_OBJECT

  public:
    PlainNotesResource( const QString &id );
    ~PlainNotesResource();

  public Q_SLOTS:
    virtual void configure( WId windowId );

  protected Q_SLOTS:
    void retrieveCollections();
    void retrieveItems( const Akonadi::Collection &col );
    bool retrieveItem( const Akonadi::Item &item, const QSet<QByteArray> &parts );

  protected:
    virtual void aboutToQuit();

    virtual void itemAdded( const Akonadi::Item &item, const Akonadi::Collection &collection );
    virtual void itemChanged( const Akonadi::Item &item, const QSet<QByteArray> &parts );
    virtual void itemRemoved( const Akonadi::Item &item );
    
    virtual void collectionAdded( const Akonadi::Collection &collection, const Akonadi::Collection &parent );
    virtual void collectionChanged( const Akonadi::Collection &collection );
    // do not hide the other variant, use implementation from base class
    // which just forwards to the one above
    using Akonadi::AgentBase::ObserverV2::collectionChanged;
    virtual void collectionRemoved( const Akonadi::Collection &collection );

    virtual void itemMoved( const Akonadi::Item &item, const Akonadi::Collection &collectionSource,
                            const Akonadi::Collection &collectionDestination );
    virtual void collectionMoved( const Akonadi::Collection &collection, const Akonadi::Collection &collectionSource,
                                  const Akonadi::Collection &collectionDestination );
    
  private:
    void saveItem( const Akonadi::Item &item, const Akonadi::Collection &parentCollection, bool saveHead, bool saveBody );
    void initializeDirectory( const QString &path ) const;
    Akonadi::Collection::List createCollectionsForDirectory( const QDir &parentDirectory, const Akonadi::Collection &parentCollection ) const;
    Akonadi::Collection::Rights supportedRights( bool isResourceCollection ) const;
    QString directoryForCollection( const Akonadi::Collection &collection ) const;
    bool removeDirectory( const QDir &directory );
    
    QString baseDirectoryPath() const;
    
  private:
    PlainNotesResourceSettings * mSettings;
    
    QString mItemMimeType;
    QStringList mSupportedMimeTypes;
};

#endif
