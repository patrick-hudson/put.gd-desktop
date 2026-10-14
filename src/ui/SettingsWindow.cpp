#include "SettingsWindow.h"
#include "storage/proton/ProtonProvider.h"
#include <QDesktopServices>
#include <QUrl>
#include "ViewState.h"
#include <QTimer>
#include <QSignalBlocker>
#include "ShortcutEditor.h"
#include "ByteSize.h"
#include "VisualStyle.h"
#include <QApplication>
#include <QAbstractItemModel>
#include <QListWidget>
#include <QStackedWidget>
#include <QToolButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QCompleter>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
namespace putgd {
namespace {
constexpr int protonFolderNameRole=Qt::UserRole+1;
void addProtonFolder(QComboBox* combo,const QString& uid,const QString& name){
 combo->addItem(name.isEmpty()?(uid.isEmpty()?"Choose a folder after connecting":"Saved Proton folder"):name,uid);
 combo->setItemData(combo->count()-1,name,protonFolderNameRole);
}
QString title(const QString& s){return s.left(1).toUpper()+s.mid(1);}
QComboBox* choices(QWidget* parent,const QString& name,const QStringList& values){auto* c=VisualStyle::choiceBox(parent);c->setObjectName(name);c->setAccessibleName(name);c->addItems(values);return c;}
QLineEdit* input(QWidget* parent,const QString& name){auto* e=new QLineEdit(parent);e->setObjectName(name);e->setAccessibleName(name);return e;}
QJsonObject s3Profile(const QString& id){
 return {{"id",id},{"label","S3 destination"},{"provider","s3"},
  {"settings",QJsonObject{{"bucket",""},{"region","us-east-1"},{"endpoint","https://s3.us-east-1.amazonaws.com"},{"objectPrefix",""},{"storageClass","STANDARD"},{"aclMode","omit"},{"publicUrlBase",QJsonValue::Null},{"addressing","auto"},{"connectTimeoutMs",5000},{"requestTimeoutMs",60000},{"lowSpeedBytesPerSecond",1},{"lowSpeedTimeSeconds",30}}},
  {"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/"+id}}}};
}
QStringList s3Regions(){
 // Offline suggestions from the AWS S3 regional endpoint table, checked 2026-10-08.
 return {"us-east-1","us-east-2","us-west-1","us-west-2","af-south-1","ap-east-1","ap-east-2","ap-south-1","ap-south-2","ap-southeast-1","ap-southeast-2","ap-southeast-3","ap-southeast-4","ap-southeast-5","ap-southeast-6","ap-southeast-7","ap-northeast-1","ap-northeast-2","ap-northeast-3","ca-central-1","ca-west-1","eu-central-1","eu-central-2","eu-west-1","eu-west-2","eu-west-3","eu-south-1","eu-south-2","eu-north-1","il-central-1","mx-central-1","me-south-1","me-central-1","sa-east-1","us-gov-east-1","us-gov-west-1"};
}
}
SettingsWindow::SettingsWindow(UiSession& settings,QWidget* parent):RetainedWindow(parent),settings_(settings) {
 setObjectName("putgdWindow");VisualStyle::apply(*qApp);setWindowIcon(VisualStyle::icon("brand"));
 setWindowTitle("put.gd — Settings");resize(1000,700);
 auto* layout=new QVBoxLayout(this);layout->setContentsMargins(20,18,20,18);layout->setSpacing(12);
 layout->addWidget(VisualStyle::header("Settings","",this));
 status_=new QLabel(this);status_->setObjectName("settingsStatus");status_->setAccessibleName("Settings status");status_->setWordWrap(true);VisualStyle::role(status_,"status");layout->addWidget(status_);
 const QString localHelp="Sets all four actions—screen region, active window, full desktop and clipboard—to save locally instead of uploading. Save settings to apply this choice. Files use the default folder or its image, video, text and other overrides; local results are copied to the clipboard. This button changes the draft; it does not capture or upload anything.";
 auto* setupActions=new QHBoxLayout;auto* advanced=new QToolButton(this);advanced->setObjectName("advancedSettings");advanced->setAccessibleName("Advanced settings");advanced->setText("Advanced settings");advanced->setCheckable(true);advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);advanced->setArrowType(Qt::RightArrow);advanced->setToolTip("Show action destinations, local folders, naming, shortcuts, credentials and resource limits. Expanding or collapsing this view preserves your unsaved changes.");setupActions->addWidget(advanced);setupActions->addStretch();auto* simpleLocal=new QPushButton("Save locally",this);simpleLocal->setObjectName("setupLocalSimple");setupActions->addWidget(simpleLocal);setupActions->addWidget(VisualStyle::helpButton("Save locally",localHelp,simpleLocal,this));layout->addLayout(setupActions);
 auto* content=new QHBoxLayout;content->setSpacing(14);layout->addLayout(content,1);
 auto* navigation=new QListWidget(this);navigation->setObjectName("settingsNavigation");navigation->setAccessibleName("Settings sections");navigation->setFixedWidth(156);navigation->setIconSize(QSize(22,22));content->addWidget(navigation);
 auto* pages=new QStackedWidget(this);pages->setObjectName("settingsPages");content->addWidget(pages,1);
 connect(navigation,&QListWidget::currentRowChanged,pages,&QStackedWidget::setCurrentIndex);
 QVBoxLayout* pageLayout=nullptr;
 auto page=[&](const QString& name,const QString& icon){auto* body=new QWidget;pageLayout=new QVBoxLayout(body);pageLayout->setContentsMargins(0,0,8,0);pageLayout->setSpacing(12);pageLayout->setAlignment(Qt::AlignTop);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(body);pages->addWidget(scroll);navigation->addItem(new QListWidgetItem(VisualStyle::icon(icon),name));};
 auto section=[&](const QString& heading,const QString& description){auto* panel=VisualStyle::card(heading,description,pages);pageLayout->addWidget(panel);auto* form=new QFormLayout;VisualStyle::styleForm(form);qobject_cast<QVBoxLayout*>(panel->layout())->addLayout(form);return form;};
 auto setting=[this](QFormLayout* form,const QString& label,const QString& help,QWidget* control,QWidget* field=nullptr){form->addRow(VisualStyle::settingLabel(label,help,control,this),field?field:control);};
 page("Capture","capture");
 auto* actionPanel=new QWidget(pages);pageLayout->addWidget(actionPanel);
 auto* actionLayout=new QVBoxLayout(actionPanel);actionLayout->setContentsMargins(0,0,0,0);actionLayout->setSpacing(12);
 auto* actionHeader=new QHBoxLayout;actionHeader->addStretch();
 auto* local=new QPushButton("Save locally",this);local->setObjectName("setupLocal");actionHeader->addWidget(local);actionHeader->addWidget(VisualStyle::helpButton("Save locally",localHelp,local,this));actionLayout->addLayout(actionHeader);
 auto* actionGrid=new QGridLayout;actionGrid->setHorizontalSpacing(12);actionGrid->setVerticalSpacing(12);actionGrid->setColumnStretch(0,1);actionGrid->setColumnStretch(1,1);actionLayout->addLayout(actionGrid);
 const QHash<QString,QString> actionLabels{{"region","Screen region"},{"window","Active window"},{"desktop","Full desktop"},{"clipboard","Clipboard"}};
 int actionIndex=0;
 for(const auto& name:Settings::actionNames()){
  auto* panel=VisualStyle::card(actionLabels.value(name),"",pages);actionGrid->addWidget(panel,actionIndex/2,actionIndex%2);++actionIndex;
  auto* actionForm=new QFormLayout;VisualStyle::styleForm(actionForm);qobject_cast<QVBoxLayout*>(panel->layout())->addLayout(actionForm);
  destinations_[name]=choices(this,"destination"+title(name),{"upload","local"});setting(actionForm,"Output","Controls what happens after the "+actionLabels.value(name).toLower()+" action. Upload sends the result to its selected destination and copies the share link; local saves it to your local folders and copies the local result. The default is upload. Changes apply to new operations after saving.",destinations_[name]);
  publicationModes_[name]=choices(this,"publication"+title(name),{"default","share","private"});
  for(int i=0;i<publicationModes_[name]->count();++i){publicationModes_[name]->setItemData(i,publicationModes_[name]->itemText(i));}
  publicationModes_[name]->setItemText(0,"Use destination default");publicationModes_[name]->setItemText(1,"Share and copy link");publicationModes_[name]->setItemText(2,"Private upload");

  connect(publicationModes_[name],&QComboBox::currentIndexChanged,this,&SettingsWindow::dirty);
  actionProfiles_[name]=choices(this,"profile"+title(name),{});actionProfiles_[name]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);actionProfiles_[name]->setMinimumContentsLength(10);setting(actionForm,"Destination","Selects the saved storage destination used by the "+actionLabels.value(name).toLower()+" action when Output is upload. Each action can use a different destination. This selection is ignored for local output, and creating a destination does not upload any files.",actionProfiles_[name]);
  auto* run=new QPushButton(name=="clipboard"?"Use clipboard":"Capture",this);run->setAccessibleName(name=="clipboard"?"Use clipboard":"Capture "+actionLabels.value(name).toLower());run->setToolTip("Run "+name+" with saved settings");run->setIcon(VisualStyle::icon(name));run->setObjectName("invoke"+title(name));actionForm->addRow(run);connect(run,&QPushButton::clicked,this,[this,name]{emit actionRequested(name);});
 }
 auto* publicationForm=section("Sharing","");for(const auto& name:Settings::actionNames()){
  setting(publicationForm,actionLabels.value(name),"Uses the destination's default sharing behavior, or overrides it for this app action. Private uploads create no share link and leave the clipboard unchanged. Providers without private-upload support reject that mode before uploading.",publicationModes_[name]);
 }
 auto* actions=section("Local files","");
 auto folderRow=[this](QLineEdit* field,const QString& label,auto choose){auto* row=new QWidget(this);auto* rowLayout=new QHBoxLayout(row);rowLayout->setContentsMargins(0,0,0,0);rowLayout->setSpacing(8);rowLayout->addWidget(field,1);auto* browse=new QPushButton("Browse…",row);browse->setAccessibleName("Choose "+label+" folder");rowLayout->addWidget(browse);connect(browse,&QPushButton::clicked,this,choose);return row;};
 root_=input(this,"archiveRoot");setting(actions,"Default folder","The absolute folder path for local captures and clipboard results, and for optional local copies of uploads. The default is the put.gd folder in Documents, or your home folder when Documents is unavailable. A category folder below overrides this folder for that content type.",root_,folderRow(root_,"default",[this]{auto folder=QFileDialog::getExistingDirectory(this,"Local output folder",root_->text());if(!folder.isEmpty())root_->setText(folder);}));
 keep_=new QCheckBox("Keep local copies of uploads",this);keep_->setObjectName("keepUploadCopies");actions->addRow(VisualStyle::settingRow("Keep local copies of uploads","Also saves an upload's captured or imported content in your local folders. Disabled by default. Local-only actions always save locally regardless of this option. A failed local copy is reported separately from the remote upload, so a successful upload is not reported as a successful local copy.",keep_,this));
 for(const auto& category:QStringList{"images","videos","text","other"}){folders_[category]=input(this,"folder"+title(category));folders_[category]->setPlaceholderText("Default folder");const auto description="An optional absolute folder path for "+category+" content. Leave it empty to use the default folder. It applies to local actions and, when enabled, local copies of uploads. Existing files are preserved by choosing an available local file name."+(category=="videos"?QString(" Video files can be imported; recording is not included."):QString{});setting(actions,title(category),description,folders_[category],folderRow(folders_[category],category,[this,category]{auto folder=QFileDialog::getExistingDirectory(this,"Local "+category+" folder",folders_[category]->text());if(!folder.isEmpty())folders_[category]->setText(folder);}));}
 actions=section("File names","");
 naming_=choices(this,"namingMode",{});naming_->addItem("Keep original names","original");naming_->addItem("Random names","generated");naming_->addItem("Custom template","template");
 collision_=choices(this,"collisionPolicy",{"rename","skip","overwrite"});
 setting(actions,"Naming","Keep original names preserves imported file names and uses the capture/image/text templates below for generated content. Random names gives every item a UUID with its extension. Custom template also renames imported files. Names are resolved once when the source is saved and stay fixed on retry. Existing configured choices are preserved.",naming_);
 setting(actions,"Duplicate names","Checks whether a remote name already exists before uploading. Rename adds a unique suffix; skip leaves that item unuploaded; overwrite replaces the checked object when the storage provider can do so safely. Rename is the default. Local files always preserve existing files and add a suffix for collisions. Overwrite can change content served by an existing link.",collision_);
 const QHash<QString,QString> templateLabels{{"fileTemplate","Imported files"},{"captureTemplate","Screen captures"},{"imageTemplate","Clipboard images"},{"textTemplate","Clipboard text"}};
 const QString tokens=" Tokens: {name} is the original stem; {ext} is the extension without a dot; {yyyy}, {MM}, {dd}, {HH}, {mm}, {ss} use local action time; {uuid} is a unique item ID. Literal text is allowed. Include the dot before {ext}. Paths and unknown tokens are rejected. Changing templates does not rename existing history or accepted jobs.";
 for(const auto& key:QStringList{"fileTemplate","captureTemplate","imageTemplate","textTemplate"}){
  templates_[key]=input(this,key);
  QString help=key=="fileTemplate"?"Used for imported file names when Custom template is selected. The default {name}.{ext} keeps the original stem and extension.":key=="captureTemplate"?"Names screen region, active-window and desktop captures in local folders and remote storage. The default Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext} produces Screen-Capture-2026-10-08-04-27-21.png.":key=="imageTemplate"?"Names images imported from the clipboard. The default Image-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext} uses a timestamp and the PNG extension.":"Names text imported from the clipboard. The default Text-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext} uses a timestamp and the txt extension.";
  setting(actions,templateLabels.value(key),help+tokens,templates_[key]);
 }
 auto templateEnabled=[this]{const auto mode=naming_->currentData().toString();for(auto it=templates_.begin();it!=templates_.end();++it)it.value()->setEnabled(it.key()=="fileTemplate"?mode=="template":mode!="generated");};
 connect(naming_,&QComboBox::currentIndexChanged,this,templateEnabled);
 page("Destinations","destinations");
 auto* destinationToolbar=new QWidget(pages);pageLayout->addWidget(destinationToolbar);auto* provider=new QFormLayout(destinationToolbar);VisualStyle::styleForm(provider);auto* profileForm=provider;profile_=choices(this,"destinationProfile",{});setting(provider,"Destination","Selects which storage destination you are editing. Multiple destinations can be assigned independently to capture and clipboard actions. Switching destinations preserves the settings draft; Save settings writes the complete draft to the configuration file.",profile_);
 auto* destinationActions=new QHBoxLayout;destinationActions->setSpacing(8);
 auto* add=new QPushButton("Add destination",this);add->setObjectName("addS3Destination");add->setIcon(VisualStyle::icon("destinations"));destinationActions->addWidget(add);
 auto* remove=new QPushButton("Remove destination",this);remove->setAccessibleName("Remove selected destination");remove->setObjectName("removeDestination");VisualStyle::role(remove,"danger");destinationActions->addWidget(remove);destinationActions->addStretch();
 auto* dev=new QPushButton("Import development S3",this);dev->setAccessibleName("Import development S3 template");dev->setToolTip("Import the development S3 template into this draft.");dev->setObjectName("importDevelopmentS3");destinationActions->addWidget(dev);provider->addRow(destinationActions);
 provider=section("Destination","");auto* destinationDetails=provider;auto* destinationEditor=provider->parentWidget();destinationEditor->setObjectName("destinationEditor");
 profileId_=input(this,"destinationId");profileLabel_=input(this,"destinationLabel");setting(provider,"Destination ID","The unique identifier used to connect actions and credential references to this destination. A new S3 destination receives an ID such as s3-1. Renaming it updates action references in this draft; it does not rename the S3 bucket or move existing uploads.",profileId_);setting(provider,"Name","The display name for this destination in action selectors. It has no effect on bucket names, remote object names or public URLs. The default name for a new destination is S3 destination.",profileLabel_);
 providerType_=choices(this,"storageProvider",{"s3","proton-drive"});for(int i=0;i<providerType_->count();++i)providerType_->setItemData(i,providerType_->itemText(i));providerType_->setItemText(0,"Amazon S3");providerType_->setItemText(1,"Proton Drive");setting(provider,"Storage service","Choose the backend for this destination. Existing history keeps its original provider and account. Proton uses a bundled SDK runtime and browser sign-in.",providerType_);
 const QHash<QString,QString> labels{{"bucket","Bucket"},{"region","Region"},{"endpoint","Endpoint"},{"objectPrefix","Object prefix"},{"storageClass","Storage class"},{"publicUrlBase","Public URL base"},{"connectTimeoutMs","Connect timeout (ms)"},{"requestTimeoutMs","Request timeout (ms)"},{"lowSpeedBytesPerSecond","Minimum transfer (bytes/s)"},{"lowSpeedTimeSeconds","Low-speed window (s)"}};
 const QHash<QString,QString> s3Help{
  {"bucket","The existing S3 bucket that receives uploads. Enter only the bucket name, without a URL or folder path. This field starts empty. Saving settings does not create a bucket or change its access policy; the saved credentials must have access to this bucket."},
  {"region","The region used to sign requests to your bucket, such as us-east-2 for Ohio. The default is us-east-1. Select an offline AWS suggestion or type a custom region for an S3-compatible service. Changing the region updates an unchanged standard AWS endpoint; it preserves a custom endpoint."},
  {"endpoint","The HTTPS address of the S3 service, without the bucket name or object prefix. It starts with the standard AWS endpoint for the selected region. Use your service's endpoint for compatible storage. A custom value is preserved when you change the region."},
  {"objectPrefix","An optional path prefix added before every remote object name, such as captures/. Leave it empty to upload at the bucket root. This does not change local folder routing and does not move previously uploaded objects."},
  {"storageClass","The S3 storage class requested for new uploads. STANDARD is the default. Other classes can have different access, retrieval and storage costs; use a value supported by your destination. This setting does not change the class of existing objects."},
  {"publicUrlBase","An optional HTTPS base URL for copied direct links, such as your own CDN or custom domain. The object path is appended to this URL. Leave it empty to use the destination's direct URL. A custom domain does not make private objects public or configure DNS or a bucket policy."},
  {"connectTimeoutMs","The connection timeout in milliseconds. The default is 5000 ms (5 seconds). It limits how long a storage request waits to establish a connection; it is separate from the overall request timeout below."},
  {"requestTimeoutMs","The request timeout in milliseconds. The default is 60000 ms (60 seconds). It controls the storage client's request timeout. Allow enough time for your connection and file sizes; a timed-out upload may still need reconciliation before its remote result is known."},
  {"lowSpeedBytesPerSecond","The minimum sustained transfer rate in bytes per second. The default is 1 byte/s. A transfer below this rate for the low-speed window is considered stalled. This is a stall threshold, not an upload speed limit or a file-size limit."},
  {"lowSpeedTimeSeconds","The number of seconds a transfer may remain below the minimum transfer rate before it is considered stalled. The default is 30 seconds. It works with Minimum transfer and does not cap an upload's total duration."}};
 for(const auto& key:QStringList{"bucket","region","endpoint","objectPrefix","storageClass","publicUrlBase","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"}){
  s3_[key]=input(this,"s3"+title(key));QWidget* field=s3_[key];
  if(key=="region"){
   auto* region=regionChoices_=choices(this,"s3RegionChoices",s3Regions());region->setEditable(true);region->setInsertPolicy(QComboBox::NoInsert);region->setLineEdit(s3_[key]);region->completer()->setCaseSensitivity(Qt::CaseInsensitive);region->completer()->setFilterMode(Qt::MatchContains);region->completer()->setCompletionMode(QCompleter::PopupCompletion);region->setAccessibleName("Region");region->setToolTip(s3Help.value(key));field=region;
  }
  setting(provider,labels.value(key),s3Help.value(key),s3_[key],field);
 }
 s3_["publicUrlBase"]->setPlaceholderText("Optional");
 acl_=choices(this,"s3AclMode",{"public-read","omit"});addressing_=choices(this,"s3Addressing",{"path","virtual","auto"});setting(provider,"Object access","Controls the ACL header sent with new uploads. Omit is the default and sends no ACL header, which suits buckets with ACLs disabled. Public-read requests a publicly readable object and requires compatible bucket settings and permission. This option does not add or modify a bucket policy.",acl_);setting(provider,"Addressing","Controls how the bucket appears in storage requests. Virtual places it in the hostname; path places it in the URL path; auto lets the storage client select a suitable form. Auto is the default. Some S3-compatible endpoints require path addressing.",addressing_);
 providerJson_=new QPlainTextEdit(this);providerJson_->setObjectName("providerSettingsJson");providerJson_->setMaximumHeight(120);setting(provider,"Provider settings","The destination-specific settings for storage providers without a dedicated form. These settings must match that provider's configuration format and are validated when saved. S3 uses the fields above, so this editor is disabled for S3 destinations.",providerJson_);
 provider=section("Credentials","");auto* credentialForm=provider;auto* credentialsEditor=provider->parentWidget();credentialsEditor->setObjectName("credentialsEditor");
 credentialMode_=choices(this,"credentialMode",{"keychain","session"});reference_=input(this,"credentialReference");setting(provider,"Credential storage","Keychain stores credentials in your desktop secret service when you click Store credentials; it is the default. Session keeps them only in memory until this app exits. Save the destination settings first, then explicitly store the credentials. The configuration contains references, never the secret keys.",credentialMode_);setting(provider,"Secret-store reference","The identifier used to find this destination's credentials in the desktop secret service. It defaults to putgd/ followed by the destination ID. It is a reference, not an access key. Session storage ignores this field and saves no reference.",reference_);
 access_=input(this,"credentialAccessKey");secret_=input(this,"credentialSecretKey");token_=input(this,"credentialSessionToken");for(auto* e:{access_,secret_,token_})e->setEchoMode(QLineEdit::Password);setting(provider,"Access key","The access key ID for the credentials authorized to use this destination. This masked input is not written to the configuration file. Save the destination first, then click Store credentials to place it in the selected credential storage. Cancel clears unsaved credential inputs.",access_);setting(provider,"Secret key","The secret access key paired with the access key ID. This masked input is not written to the configuration file or history. Save the destination first, then explicitly click Store credentials. The input is cleared after that action and when you cancel or reload the settings draft.",secret_);setting(provider,"Session token","The session token supplied with temporary AWS credentials. Leave it empty for credentials that do not include a token. Store it together with the access key and secret key after saving the destination. This masked input is cleared after storing, cancelling or reloading.",token_);
 auto* credentialActions=new QHBoxLayout;credentialActions->setSpacing(8);auto* credentials=new QPushButton("Store credentials",this);credentials->setObjectName("storeCredentials");credentials->setToolTip("Save settings before storing credentials");auto* unlock=new QPushButton("Unlock saved credentials",this);credentialActions->addWidget(credentials);credentialActions->addWidget(unlock);auto* test=new QPushButton("Test saved destination",this);test->setToolTip("Test saved destination with a synthetic object");test->setObjectName("testDestination");credentialActions->addWidget(test);credentialActions->addStretch();provider->addRow(credentialActions);
 auto* protonForm=section("Proton Drive","This is a third-party integration, not officially supported by Proton. Share links open a Proton page; they are not direct file URLs.");protonPanel_=protonForm->parentWidget();protonPanel_->setObjectName("protonDestinationEditor");
 protonAccount_=new QLabel("Connect an account after saving this destination.",this);protonAccount_->setWordWrap(true);protonForm->addRow("Account",protonAccount_);
 auto* connectProton=new QPushButton("Connect account…",this);connectProton->setObjectName("connectProtonAccount");auto* inspectProton=new QPushButton("Refresh folders",this);inspectProton->setObjectName("inspectProtonAccount");auto* accountActions=new QHBoxLayout;accountActions->addWidget(connectProton);accountActions->addWidget(inspectProton);accountActions->addStretch();protonForm->addRow(accountActions);
 protonFolder_=choices(this,"protonFolder",{});setting(protonForm,"Folder","Choose an existing top-level folder in My files or create a new folder. Private mode creates no link; existing member/folder access still applies.",protonFolder_);
 auto* createProton=new QPushButton("Create put.gd folder",this);createProton->setObjectName("createProtonFolder");protonForm->addRow(createProton);
 protonShare_=new QCheckBox("Share uploads and copy the link",this);protonShare_->setObjectName("protonShareDefault");setting(protonForm,"Default publication","Sharing is the default. Clear this to upload without creating a public link. Individual app actions can override this under Advanced settings.",protonShare_);
 protonExpiration_=new QSpinBox(this);protonExpiration_->setObjectName("protonExpiration");protonExpiration_->setRange(0,365);protonExpiration_->setSpecialValueText("No expiration");protonExpiration_->setSuffix(" days");setting(protonForm,"Link expiration","Applies to newly created links. Existing links keep their expiration until explicitly changed.",protonExpiration_);
 protonPassword_=input(this,"protonSharePassword");protonPassword_->setEchoMode(QLineEdit::Password);setting(protonForm,"New link password","Optional password for new links. This masked value is sent through private IPC to the OS keychain and never saved in configuration, history or command arguments.",protonPassword_);
 auto* storePassword=new QPushButton("Store link password",this);storePassword->setObjectName("storeProtonPassword");protonForm->addRow(storePassword);
 connect(connectProton,&QPushButton::clicked,this,[this]{protonBrowserOpened_=false;beginSetup("connectAccount");});connect(inspectProton,&QPushButton::clicked,this,[this]{beginSetup("inspectAccount");});connect(createProton,&QPushButton::clicked,this,[this]{beginSetup("createFolder",{{"name","put.gd-"+QUuid::createUuid().toString(QUuid::Id128).left(12)}});});
 connect(storePassword,&QPushButton::clicked,this,[this]{const auto reference="putgd/proton/link/"+QUuid::createUuid().toString(QUuid::Id128);auto value=protonPassword_->text();protonPassword_->clear();beginSetup("storeSharePassword",{{"password",value},{"reference",reference}});value.fill(QChar{});});
 auto* clearPassword=new QPushButton("Use no link password",this);clearPassword->setObjectName("clearProtonPassword");protonForm->addRow(clearPassword);
 connect(clearPassword,&QPushButton::clicked,this,[this]{auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return;auto p=profiles[profileIndex_].toObject();auto s=p["settings"].toObject();s["sharePasswordReference"]=QJsonValue::Null;p["settings"]=s;profiles[profileIndex_]=p;draft_["profiles"]=profiles;protonPassword_->clear();dirty();});
 connect(protonFolder_,&QComboBox::currentIndexChanged,this,&SettingsWindow::dirty);connect(protonShare_,&QCheckBox::toggled,this,&SettingsWindow::dirty);connect(protonExpiration_,&QSpinBox::valueChanged,this,&SettingsWindow::dirty);
 connect(providerType_,&QComboBox::currentIndexChanged,this,[this]{if(loading_)return;auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return;auto p=profiles[profileIndex_].toObject();p["provider"]=providerType_->currentData().toString();p["settings"]=p["provider"]=="proton-drive"?ProtonProvider::defaults():s3Profile(p["id"].toString())["settings"].toObject();p["label"]=p["provider"]=="proton-drive"?"Proton Drive":"S3 destination";if(p["provider"]=="proton-drive")p["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/proton/"+QUuid::createUuid().toString(QUuid::Id128)}};profiles[profileIndex_]=p;draft_["profiles"]=profiles;if(profileIndex_<profileEditors_.size())profileEditors_[profileIndex_]=QJsonValue::Null;loadProfile();dirty();});
 page("Shortcuts","shortcuts");
 auto* shortcutPage=section("Global shortcuts","");shortcuts_=new ShortcutEditor(this);shortcutPage->addRow(shortcuts_);
 shortcuts_->setRecordingControl([session=&settings_,editor=shortcuts_](bool active,std::function<void(Error)> reply){session->setShortcutRecording(active,editor,std::move(reply));});
 shortcutPage=section("Desktop support","Current session capabilities and bindings.");capabilities_=new QLabel(this);capabilities_->setWordWrap(true);capabilities_->setTextInteractionFlags(Qt::TextSelectableByMouse);capabilities_->setAccessibleName("Desktop support diagnostics");VisualStyle::role(capabilities_,"muted");shortcutPage->addRow(capabilities_);auto* diagnose=new QPushButton("Refresh desktop support",this);auto* diagnosticActions=new QHBoxLayout;diagnosticActions->addWidget(diagnose);diagnosticActions->addStretch();shortcutPage->addRow(diagnosticActions);connect(diagnose,&QPushButton::clicked,this,[this]{settings_.refreshSnapshot();});
 page("Preferences","preferences");
 auto* limits=section("Startup and notifications","");notifications_=new QCheckBox("Desktop notifications",this);notifications_->setObjectName("notifications");login_=new QCheckBox("Launch at login",this);login_->setObjectName("launchAtLogin");limits->addRow(VisualStyle::settingRow("Desktop notifications","Shows desktop notifications for operation results when your desktop supports them. Enabled by default. Settings confirmations still appear inside the app, and operation results remain available in Activity and History when desktop notifications are disabled.",notifications_,this));limits->addRow(VisualStyle::settingRow("Launch at login","Starts the resident put.gd app when you sign in to your desktop. Disabled by default. Enabling it uses the desktop's startup integration; disabling it removes that startup entry. Saving the choice applies it without starting another copy of the app.",login_,this));
 limits=section("Limits and retention","");
 const QHash<QString,QString> numberLabels{{"maxFiles","Files per batch"},{"pendingOperations","Pending operations"},{"concurrentUploads","Concurrent uploads"},{"maxFileBytes","File size (MiB)"},{"maxBatchBytes","Batch size (MiB)"},{"pendingImageBytes","Queued image memory (MiB)"},{"pendingArtifactBytes","Pending artifacts (MiB)"},{"clipboardTextBytes","Clipboard text (MiB)"},{"imagePixels","Image pixels"},{"imageBytes","Image size (MiB)"},{"failedArtifactDays","Failed artifacts (days)"},{"recentActivityLimit","Recent activity entries"},{"previewCacheBytes","Preview cache (MiB)"},{"historyRetentionDays","History retention (days)"}};
 const QHash<QString,QString> numberHelp{
  {"pendingImageBytes","Maximum total decoded image bytes retained by jobs waiting for or undergoing encoding. The default is 64 MiB. An acquisition that cannot fit fails with image_memory_limit; it is not silently deferred or changed. The limit is released after disk persistence or queued cancellation. This bounds source payloads, not total app RSS, compositor frames, encoding workspace or the current native clipboard representation."},
  {"maxFiles","The maximum number of files accepted from one clipboard action. The default is 100. A batch above this limit is rejected before importing its files. This is a per-action limit and does not restrict the total number of history entries."},
  {"pendingOperations","The maximum number of queued operations waiting for processing, in addition to those currently running. The default is 8. When the queue is full, a new action is rejected rather than accumulating more pending work."},
  {"concurrentUploads","The maximum number of operations processed at the same time. The default is 1. Increasing it can improve throughput but uses more memory and network resources; it does not reserve that memory in advance."},
  {"maxFileBytes","The largest individual imported file, in MiB. The default is 2048 MiB (2 GiB). Files above the limit are rejected before importing. One MiB equals 1,048,576 bytes; fractional MiB values are accepted when they represent a whole number of bytes."},
  {"maxBatchBytes","The maximum combined size of files accepted in one clipboard batch, in MiB. The default is 2048 MiB (2 GiB). This applies to the whole batch in addition to the individual file-size limit. One MiB equals 1,048,576 bytes."},
  {"pendingArtifactBytes","The maximum total size of acquired content waiting in the pending artifact store on disk, in MiB. The default is 2048 MiB (2 GiB). New imports are rejected when they would exceed this limit. This is a disk-content ceiling, not memory allocated at startup."},
  {"clipboardTextBytes","The largest text payload accepted from the clipboard, in MiB. The default is 16 MiB. Larger text is rejected before importing. One MiB equals 1,048,576 bytes; this limit does not apply to ordinary file uploads."},
  {"imagePixels","The maximum number of pixels in an accepted image, calculated as width × height. The default is 100,000,000 pixels. This protects against oversized decoded images independently of their encoded file size."},
  {"imageBytes","The maximum accepted image payload size, in MiB. The default is 512 MiB. It works alongside the image-pixel limit; an image must satisfy both limits. One MiB equals 1,048,576 bytes. This is a ceiling, not a persistent allocation."},
  {"failedArtifactDays","How many days failed or interrupted local artifacts are kept for explicit retry before cleanup may remove them. The default is 7 days. Set 0 to disable age-based removal of failed artifacts. This controls local retry content and does not delete remote objects."},
  {"recentActivityLimit","The maximum number of recent operation entries shown in Activity. The default is 100. This bounds the activity list; it does not set the complete upload-history retention period below."},
  {"previewCacheBytes","The maximum total size of generated image previews stored on disk, in MiB. The default is 256 MiB. Set 0 to disable stored previews. This is a disk-cache limit, not a memory allowance; previews may be regenerated or removed by cleanup."},
  {"historyRetentionDays","How many days completed history metadata is retained before automatic pruning. Leave empty to keep history indefinitely, which is the default. Removing old history metadata does not delete remote uploads or your saved local files."}};
 for(const auto& key:QStringList{"maxFiles","pendingOperations","concurrentUploads","maxFileBytes","maxBatchBytes","pendingArtifactBytes","pendingImageBytes","clipboardTextBytes","imagePixels","imageBytes","failedArtifactDays","recentActivityLimit","previewCacheBytes","historyRetentionDays"}){numbers_[key]=input(this,key);setting(limits,numberLabels.value(key),numberHelp.value(key),numbers_[key]);}numbers_["historyRetentionDays"]->setPlaceholderText("Indefinite");
 auto profileVisibility=[this,advanced,profileForm,add,remove,destinationEditor,credentialsEditor]{
  const bool hasDestination=profile_->count()>0;
  profileForm->setRowVisible(profile_,hasDestination&&(advanced->isChecked()||profile_->count()>1));
  destinationEditor->setVisible(hasDestination);credentialsEditor->setVisible(hasDestination);
  add->setVisible(advanced->isChecked()||!hasDestination);VisualStyle::role(add,hasDestination?"secondary":"primary");
  remove->setVisible(advanced->isChecked()&&hasDestination);
 };
 connect(profile_->model(),&QAbstractItemModel::rowsInserted,this,profileVisibility);connect(profile_->model(),&QAbstractItemModel::rowsRemoved,this,profileVisibility);
 auto alignDestinationForms=[destinationDetails,credentialForm]{
  int width=0;
  for(auto* form:{destinationDetails,credentialForm})for(int row=0;row<form->rowCount();++row){
   auto* item=form->itemAt(row,QFormLayout::LabelRole);auto* label=item?item->widget():nullptr;
   if(label){label->setMinimumWidth(0);if(!label->isHidden())width=qMax(width,label->sizeHint().width());}
  }
  for(auto* form:{destinationDetails,credentialForm})for(int row=0;row<form->rowCount();++row){
   auto* item=form->itemAt(row,QFormLayout::LabelRole);if(item&&item->widget())item->widget()->setMinimumWidth(width);
  }
 };
 updateProviderVisibility_=[this,advanced,navigation,destinationDetails,credentialForm,profileVisibility,alignDestinationForms,dev,credentialsEditor,protonForm,storePassword,clearPassword]{
  const bool expanded=advanced->isChecked(),s3=providerType_->currentData()=="s3",proton=providerType_->currentData()=="proton-drive";
  navigation->setVisible(expanded);advanced->setArrowType(expanded?Qt::DownArrow:Qt::RightArrow);
  for(auto* widget:{profileId_,profileLabel_})destinationDetails->setRowVisible(widget,expanded);
  for(auto it=s3_.begin();it!=s3_.end();++it)destinationDetails->setRowVisible(it.key()=="region"?static_cast<QWidget*>(regionChoices_):it.value(),s3&&(expanded||QStringList{"bucket","region","publicUrlBase"}.contains(it.key())));
  destinationDetails->setRowVisible(acl_,s3&&expanded);destinationDetails->setRowVisible(addressing_,s3&&expanded);destinationDetails->setRowVisible(providerJson_,false);
  credentialForm->setRowVisible(reference_,expanded);credentialForm->setRowVisible(credentialMode_,expanded);credentialForm->setRowVisible(token_,expanded);
  protonForm->setRowVisible(protonExpiration_,expanded);protonForm->setRowVisible(protonPassword_,expanded);protonForm->setRowVisible(storePassword,expanded);protonForm->setRowVisible(clearPassword,expanded);
  profileVisibility();credentialsEditor->setVisible(profile_->count()>0&&s3);protonPanel_->setVisible(profile_->count()>0&&proton);alignDestinationForms();dev->setVisible(expanded);
 };
 connect(advanced,&QToolButton::toggled,this,[this,navigation](bool expanded){if(!expanded)navigation->setCurrentRow(1);updateProviderVisibility_();});navigation->setCurrentRow(1);updateProviderVisibility_();
 auto* configuration=new QToolButton(this);configuration->setText("Configuration details");configuration->setCheckable(true);configuration->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);configuration->setArrowType(Qt::RightArrow);layout->addWidget(configuration);
 metadata_=new QLabel(this);metadata_->setObjectName("settingsMetadata");metadata_->setWordWrap(true);metadata_->setTextInteractionFlags(Qt::TextSelectableByMouse);VisualStyle::role(metadata_,"muted");metadata_->hide();layout->addWidget(metadata_);
 connect(configuration,&QToolButton::toggled,this,[configuration,this](bool visible){metadata_->setVisible(visible);configuration->setArrowType(visible?Qt::DownArrow:Qt::RightArrow);});
 auto* buttons=new QHBoxLayout;buttons->setSpacing(10);auto* cancelButton=new QPushButton("Cancel",this);cancelButton->setObjectName("cancelSettings");cancelButton->setToolTip("Discard unsaved settings and credential inputs, then close Settings. Previously saved settings and stored credentials remain active.");buttons->addWidget(cancelButton);quit_=new QPushButton("Quit put.gd",this);quit_->setObjectName("quitApplication");quit_->setToolTip("Exit put.gd, stop shortcuts and cancel pending operations. Unsaved settings are discarded. Closing Settings alone keeps put.gd running.");buttons->addWidget(quit_);buttons->addStretch();save_=new QPushButton("Save settings",this);save_->setObjectName("saveSettings");save_->setToolTip("Validate and save this draft to the configuration file. Changes apply to new operations. Secret keys are stored separately with Store credentials.");VisualStyle::role(save_,"primary");auto* reload=new QPushButton("Reload from file",this);reload->setToolTip("Discard unsaved settings and credential inputs, then read the configuration file again. If the file is invalid, the last accepted settings remain active and the error is shown here.");reload->setObjectName("reloadSettings");buttons->addWidget(reload);buttons->addWidget(VisualStyle::helpButton("Reload from file",reload->toolTip(),reload,this));buttons->addWidget(save_);layout->addLayout(buttons);
 connect(quit_,&QPushButton::clicked,this,&SettingsWindow::quitRequested);
 connect(cancelButton,&QPushButton::clicked,this,&SettingsWindow::cancel);connect(save_,&QPushButton::clicked,this,&SettingsWindow::save);connect(reload,&QPushButton::clicked,this,[this]{resetDraft(true);});
 connect(&settings_,&UiSession::accepted,this,[this]{configurationChanged();});connect(&settings_,&UiSession::statusChanged,this,[this]{configurationChanged();refreshDiagnostics();});
 connect(simpleLocal,&QPushButton::clicked,local,&QPushButton::click);
 connect(local,&QPushButton::clicked,this,[this]{for(auto* c:destinations_)c->setCurrentText("local");if(initialProfile_){draft_["profiles"]=QJsonArray{};profileEditors_={};initialProfile_=false;rebuildProfiles(-1);access_->clear();secret_->clear();token_->clear();}dirty();VisualStyle::feedback(status_,"Local output selected. Save settings to apply.");});
 connect(dev,&QPushButton::clicked,this,[this]{if(initialProfile_&&s3_["bucket"]->text().isEmpty()){draft_["profiles"]=QJsonArray{};profileEditors_={};profileIndex_=-1;for(auto* combo:actionProfiles_)combo->setCurrentIndex(0);}else storeProfile();draft_=candidate();auto profiles=draft_["profiles"].toArray();auto development=Settings::developmentTemplate()["profiles"].toArray().first();int index=-1;for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]=="development-s3")index=i;if(index<0){index=profiles.size();profiles.append(development);}draft_["profiles"]=profiles;initialProfile_=false;rebuildProfiles(index,"development-s3");dirty();});
 connect(add,&QPushButton::clicked,this,&SettingsWindow::addS3Profile);
 connect(remove,&QPushButton::clicked,this,[this]{storeProfile();draft_=candidate();auto profiles=draft_["profiles"].toArray();const int index=profile_->currentIndex();if(index<0||index>=profiles.size())return;profiles.removeAt(index);if(index<profileEditors_.size())profileEditors_.removeAt(index);draft_["profiles"]=profiles;rebuildProfiles(qMin(index,int(profiles.size())-1));dirty();VisualStyle::feedback(status_,"Destination removed from draft. Reassign affected actions before saving. History and remote objects remain.");});
 connect(s3_["region"],&QLineEdit::textChanged,this,[this](const QString& region){if(loading_)return;auto* endpoint=s3_["endpoint"];const auto previous=s3_["region"]->property("previousRegion").toString();if(endpoint->text()=="https://s3."+previous+".amazonaws.com")endpoint->setText("https://s3."+region+".amazonaws.com");s3_["region"]->setProperty("previousRegion",region);});
 connect(profile_,&QComboBox::currentIndexChanged,this,[this]{if(!loading_)storeProfile();loadProfile();});
 for(auto* e:findChildren<QLineEdit*>()) {
  if(e!=access_&&e!=secret_&&e!=token_&&e!=protonPassword_)connect(e,&QLineEdit::textChanged,this,[this]{dirty();});
 }
 for(auto* c:findChildren<QComboBox*>())connect(c,&QComboBox::currentIndexChanged,this,[this]{dirty();});
 for(auto* c:findChildren<QCheckBox*>())connect(c,&QCheckBox::toggled,this,[this]{dirty();});
 connect(providerJson_,&QPlainTextEdit::textChanged,this,[this]{dirty();});
 connect(credentials,&QPushButton::clicked,this,[this]{QJsonObject input{{"accessKey",access_->text()},{"secretKey",secret_->text()},{"sessionToken",token_->text()}};clearCredentialInputs();beginSetup("storeCredentials",input);});
 connect(unlock,&QPushButton::clicked,this,[this]{beginSetup("unlockCredentials");});
 connect(test,&QPushButton::clicked,this,[this]{beginSetup("testDestination");});
 setupTimer_=new QTimer(this);setupTimer_->setInterval(500);connect(setupTimer_,&QTimer::timeout,this,&SettingsWindow::pollSetup);
 connect(advanced,&QToolButton::toggled,this,&SettingsWindow::viewStateChanged);connect(navigation,&QListWidget::currentRowChanged,this,&SettingsWindow::viewStateChanged);
 refresh();refreshDiagnostics();
}
void SettingsWindow::refreshMetadata(){metadata_->setText("Configuration: "+settings_.path()+"\nAccepted revision: "+settings_.revision()+"\nObserved file revision: "+settings_.metadata()["observedRevision"].toString());}
void SettingsWindow::dirty(){if(loading_)return;dirty_=true;++editGeneration_;emit viewStateChanged();}
void SettingsWindow::status(const Error& e){
 if(e.code=="config_missing"&&settings_.revision().isEmpty()){VisualStyle::feedback(status_,"Changes apply after saving.");return;}
 VisualStyle::feedback(status_,e?e.code+": "+e.message+" "+e.details["path"].toString():"Done.",bool(e));
}
void SettingsWindow::refreshDiagnostics(){auto data=settings_.diagnostics();quit_->setVisible(data["trayAvailable"].isBool()&&!data["trayAvailable"].toBool());capabilities_->setText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)));shortcuts_->setStatus(data["shortcuts"].toObject());}
void SettingsWindow::refresh(){
 loading_=true;draft_=settings_.document().isEmpty()?Settings::defaults():settings_.document();profileEditors_={};baseRevision_=settings_.revision();observed_=settings_.metadata()["observedRevision"].toString();dirty_=conflict_=false;save_->setEnabled(true);refreshMetadata();
 VisualStyle::feedback(status_,settings_.revision().isEmpty()?"Changes apply after saving.":"Saved settings are active.");
 auto actions=draft_["actions"].toObject();auto profiles=draft_["profiles"].toArray();initialProfile_=settings_.revision().isEmpty()&&profiles.isEmpty();
 if(initialProfile_){
  const QString id="s3-1";profiles.append(s3Profile(id));draft_["profiles"]=profiles;
  for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();if(action["destination"]=="upload"&&action["profileId"].toString().isEmpty())action["profileId"]=id;actions[name]=action;}
  draft_["actions"]=actions;
 }
 populate();
}
void SettingsWindow::populate(){
 loading_=true;const auto profiles=draft_["profiles"].toArray();const auto actions=draft_["actions"].toObject();
 auto selected=profile_->currentText();profileIndex_=-1;profile_->clear();for(auto p:profiles)profile_->addItem(p.toObject()["id"].toString());int selectedIndex=profile_->findText(selected);if(selectedIndex>=0)profile_->setCurrentIndex(selectedIndex);loadProfile();
 for(const auto& name:Settings::actionNames()){
  destinations_[name]->setCurrentText(actions[name].toObject()["destination"].toString());publicationModes_[name]->setCurrentIndex(publicationModes_[name]->findData(actions[name].toObject()["publicationMode"].toString("default")));auto* c=actionProfiles_[name];c->clear();c->addItem("No destination",QString{});for(auto p:profiles)c->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());c->setCurrentIndex(c->findData(actions[name].toObject()["profileId"].toString()));
 }
 shortcuts_->setActions(actions);auto archive=draft_["archive"].toObject();root_->setText(archive["root"].toString());keep_->setChecked(archive["keepUploads"].toBool());for(auto it=folders_.begin();it!=folders_.end();++it)it.value()->setText(archive["folders"].toObject()[it.key()].toString());
 naming_->setCurrentIndex(naming_->findData(draft_["naming"].toObject()["mode"].toString()));const auto effective=Settings::fromValidated(draft_);const QHash<QString,QString> formats{{"fileTemplate",effective.fileTemplate},{"captureTemplate",effective.captureTemplate},{"imageTemplate",effective.imageTemplate},{"textTemplate",effective.textTemplate}};for(auto it=templates_.begin();it!=templates_.end();++it)it.value()->setText(formats.value(it.key()));collision_->setCurrentText(draft_["naming"].toObject()["collisionPolicy"].toString());auto prefs=draft_["preferences"].toObject();notifications_->setChecked(prefs["notifications"].toBool());login_->setChecked(prefs["launchAtLogin"].toBool());
 for(auto it=numbers_.begin();it!=numbers_.end();++it){auto value=draft_["limits"].toObject().contains(it.key())?draft_["limits"].toObject()[it.key()]:draft_["retention"].toObject()[it.key()];if(it.key()=="pendingImageBytes"&&value.isUndefined())value=effective.limits.pendingImageBytes;it.value()->setText(value.isNull()?QString{}:(it.key().endsWith("Bytes")?ByteSize::exact(value.toInteger()):QString::number(value.toInteger())));}
 loading_=false;
}
void SettingsWindow::addS3Profile(){
 storeProfile();draft_=candidate();auto profiles=draft_["profiles"].toArray();if(profiles.size()>=32){status({"profile_limit","At most 32 destinations are supported",{}});return;}
 QString id;int number=1;do{id="s3-"+QString::number(number++);}while(std::any_of(profiles.begin(),profiles.end(),[&](const QJsonValue& p){return p.toObject()["id"].toString()==id;}));
 profiles.append(s3Profile(id));draft_["profiles"]=profiles;initialProfile_=false;rebuildProfiles(profiles.size()-1);
 for(const auto& name:Settings::actionNames())if(destinations_[name]->currentText()=="upload"&&actionProfiles_[name]->currentData().toString().isEmpty())actionProfiles_[name]->setCurrentIndex(actionProfiles_[name]->findData(id));
 dirty();
}
void SettingsWindow::cancel(){resetDraft(false);}
void SettingsWindow::resetDraft(bool reloadFile){
 if(resetting_||saving_||setupAccepting_)return;
 clearCredentialInputs();resetting_=true;setEnabled(false);emit viewStateChanged();
 const auto finish=[g=QPointer<SettingsWindow>(this),reloadFile](Error error){
  if(!g)return;
  g->resetting_=false;g->setEnabled(true);
  if(error){g->configurationChanged();g->status(error);emit g->viewStateChanged();return;}
  g->refresh();emit g->viewStateChanged();
  if(reloadFile)VisualStyle::feedback(g->status_,"Settings reloaded from file.");else g->close();
 };
 if(reloadFile)settings_.reloadAsync([finish](Result result){finish(result.error);});
 else settings_.refreshSnapshot(finish);
}

void SettingsWindow::rebuildProfiles(int selected,const QString& defaultProfile){const bool previous=loading_;loading_=true;const auto profiles=draft_["profiles"].toArray();profileIndex_=-1;profile_->clear();for(auto p:profiles)profile_->addItem(p.toObject()["id"].toString());profile_->setCurrentIndex(selected);loadProfile();for(auto* combo:actionProfiles_){auto id=combo->currentData().toString();combo->clear();combo->addItem("No destination",QString{});for(auto p:profiles)combo->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());int index=combo->findData(id.isEmpty()?defaultProfile:id);combo->setCurrentIndex(index<0?0:index);}loading_=previous;}
void SettingsWindow::loadProfile(){bool previous=loading_;loading_=true;profileIndex_=profile_->currentIndex();auto profiles=draft_["profiles"].toArray();auto p=profileIndex_>=0&&profileIndex_<profiles.size()?profiles[profileIndex_].toObject():QJsonObject{};profileId_->setText(p["id"].toString());profileLabel_->setText(p["label"].toString());const auto s=p["settings"].toObject();bool s3=p["provider"]=="s3";
 providerType_->setCurrentIndex(providerType_->findData(p["provider"].toString()));const bool proton=p["provider"]=="proton-drive";protonPanel_->setVisible(proton);protonAccountUid_=s["accountRootUid"].toString();protonAccount_->setText(protonAccountUid_.isEmpty()?"Not connected":"Connected account; refresh folders to select a destination.");protonShare_->setChecked(s["shareByDefault"].toBool(true));protonExpiration_->setValue(s["linkExpirationDays"].toInt());protonFolder_->clear();addProtonFolder(protonFolder_,s["folderUid"].toString(),s["folderName"].toString());
 findChild<QPushButton*>("storeCredentials")->setVisible(s3);findChild<QPushButton*>("testDestination")->setVisible(s3);access_->setVisible(s3);secret_->setVisible(s3);token_->setVisible(s3);for(auto it=s3_.begin();it!=s3_.end();++it){auto v=s[it.key()];it.value()->setText(v.isDouble()?QString::number(v.toInteger()):v.toString());it.value()->setEnabled(s3);}s3_["region"]->setProperty("previousRegion",s["region"].toString());regionChoices_->setEnabled(s3);acl_->setEnabled(s3);addressing_->setEnabled(s3);acl_->setCurrentText(s["aclMode"].toString());addressing_->setCurrentText(s["addressing"].toString());providerJson_->setEnabled(!s3);providerJson_->setPlainText(proton?QString{}:QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Indented)));auto ref=p["credentials"].toObject();credentialMode_->setCurrentText(ref["mode"].toString());reference_->setText(ref["reference"].toString());if(profileIndex_>=0&&profileIndex_<profileEditors_.size()&&profileEditors_[profileIndex_].isObject())applyProfileEditor(profileEditors_[profileIndex_].toObject());if(updateProviderVisibility_)updateProviderVisibility_();loading_=previous;}
QJsonObject SettingsWindow::editedProfile()const{auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return {};auto p=profiles[profileIndex_].toObject();p["id"]=profileId_->text();p["label"]=profileLabel_->text();auto s=p["settings"].toObject();if(p["provider"]=="s3"){for(auto it=s3_.begin();it!=s3_.end();++it){if(it.key().endsWith("Ms")||it.key().startsWith("lowSpeed")){bool ok=false;auto value=it.value()->text().toLongLong(&ok);s[it.key()]=ok?QJsonValue(value):QJsonValue(it.value()->text());}else if(it.key()=="publicUrlBase"&&it.value()->text().isEmpty())s[it.key()]=QJsonValue::Null;else s[it.key()]=it.value()->text();}s["aclMode"]=acl_->currentText();s["addressing"]=addressing_->currentText();}else if(p["provider"]=="proton-drive"){s["accountRootUid"]=protonAccountUid_;s["folderUid"]=protonFolder_->currentData().toString();const auto folderName=protonFolder_->currentData(protonFolderNameRole).toString();if(folderName.isEmpty())s.remove("folderName");else s["folderName"]=folderName;s["shareByDefault"]=protonShare_->isChecked();s["linkExpirationDays"]=protonExpiration_->value();}else {auto parsed=QJsonDocument::fromJson(providerJson_->toPlainText().toUtf8());if(parsed.isObject())s=parsed.object();else s={{"invalidJson",true}};}p["settings"]=s;p["credentials"]=QJsonObject{{"mode",credentialMode_->currentText()},{"reference",credentialMode_->currentText()=="session"?QJsonValue(QJsonValue::Null):QJsonValue(reference_->text())}};return p;}
void SettingsWindow::storeProfile(){
 auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return;
 while(profileEditors_.size()<profiles.size())profileEditors_.append(QJsonValue::Null);
 profileEditors_[profileIndex_]=profileEditorState();
 const auto oldId=profiles[profileIndex_].toObject()["id"].toString();const auto p=editedProfile();
 if(oldId!=p["id"].toString()){for(auto* combo:actionProfiles_){int index=combo->findData(oldId);if(index>=0){combo->setItemData(index,p["id"].toString());combo->setItemText(index,p["label"].toString());}}profile_->setItemText(profileIndex_,p["id"].toString());}
 profiles[profileIndex_]=p;draft_["profiles"]=profiles;
}
QJsonObject SettingsWindow::candidate() const {auto doc=draft_;auto profiles=doc["profiles"].toArray();if(profileIndex_>=0&&profileIndex_<profiles.size())profiles[profileIndex_]=editedProfile();doc["profiles"]=profiles;auto actions=shortcuts_->actions(doc["actions"].toObject());for(const auto& name:Settings::actionNames()){auto a=actions[name].toObject();a["destination"]=destinations_[name]->currentText();if(publicationModes_[name]->currentData()!="default"||a.contains("publicationMode"))a["publicationMode"]=publicationModes_[name]->currentData().toString();auto id=actionProfiles_[name]->currentData().toString();if(profileIndex_>=0&&profileIndex_<draft_["profiles"].toArray().size()&&id==draft_["profiles"].toArray()[profileIndex_].toObject()["id"].toString())id=profileId_->text();a["profileId"]=id.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(id);actions[name]=a;}doc["actions"]=actions;auto archive=doc["archive"].toObject();archive["root"]=root_->text();archive["keepUploads"]=keep_->isChecked();QJsonObject folders;for(auto it=folders_.begin();it!=folders_.end();++it)folders[it.key()]=it.value()->text().isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(it.value()->text());archive["folders"]=folders;doc["archive"]=archive;auto naming=doc["naming"].toObject();naming["mode"]=naming_->currentData().toString();naming["collisionPolicy"]=collision_->currentText();for(auto it=templates_.begin();it!=templates_.end();++it)naming[it.key()]=it.value()->text();doc["naming"]=naming;doc["preferences"]=QJsonObject{{"notifications",notifications_->isChecked()},{"launchAtLogin",login_->isChecked()}};auto limits=doc["limits"].toObject(),retention=doc["retention"].toObject();for(auto it=numbers_.begin();it!=numbers_.end();++it){bool ok=false;auto n=it.value()->text().toLongLong(&ok);QJsonValue value=it.key().endsWith("Bytes")?ByteSize::fromMiB(it.value()->text()):(ok?QJsonValue(n):QJsonValue(it.value()->text()));if(it.key()=="historyRetentionDays"&&it.value()->text().isEmpty())value=QJsonValue::Null;if(limits.contains(it.key())||it.key()=="pendingImageBytes")limits[it.key()]=value;else retention[it.key()]=value;}doc["limits"]=limits;doc["retention"]=retention;return doc;}
void SettingsWindow::save(){if(conflict_||saving_||resetting_)return;saving_=true;storeProfile();auto doc=candidate();const auto edits=editGeneration_;save_->setEnabled(false);settings_.saveAsync(doc,observed_,[g=QPointer<SettingsWindow>(this),edits](Result result){if(!g)return;g->saving_=false;if(result){if(g->editGeneration_==edits){g->refresh();VisualStyle::success(g->status_,"✓  Settings saved. Changes apply to new operations.");}else{g->baseRevision_=g->settings_.revision();g->observed_=g->settings_.metadata()["observedRevision"].toString();g->save_->setEnabled(true);g->refreshMetadata();VisualStyle::success(g->status_,"✓  Settings saved. Your newer edits remain unsaved.");}}else {g->save_->setEnabled(true);g->status(result.error);if(result.error.code=="config_conflict"){g->conflict_=true;g->save_->setEnabled(false);}}emit g->viewStateChanged();});}
QJsonObject SettingsWindow::profileEditorState()const{
 QJsonObject s3;for(auto it=s3_.begin();it!=s3_.end();++it)s3[it.key()]=it.value()->text();
 return {{"id",profileId_->text()},{"label",profileLabel_->text()},{"credentialMode",credentialMode_->currentText()},{"reference",reference_->text()},{"acl",acl_->currentText()},{"addressing",addressing_->currentText()},{"s3",s3},{"providerJson",providerJson_->toPlainText()}};
}
void SettingsWindow::applyProfileEditor(const QJsonObject& state){
 profileId_->setText(state["id"].toString());profileLabel_->setText(state["label"].toString());credentialMode_->setCurrentText(state["credentialMode"].toString());reference_->setText(state["reference"].toString());acl_->setCurrentText(state["acl"].toString());addressing_->setCurrentText(state["addressing"].toString());const auto s3=state["s3"].toObject();for(auto it=s3_.begin();it!=s3_.end();++it)it.value()->setText(s3[it.key()].toString());s3_["region"]->setProperty("previousRegion",s3_["region"]->text());providerJson_->setPlainText(state["providerJson"].toString());
}
QJsonObject SettingsWindow::saveViewState()const{
 if(saving_||setupAccepting_||resetting_)return {{"stateError","request_awaiting_acceptance"}};
 QJsonObject numbers;for(auto it=numbers_.begin();it!=numbers_.end();++it)numbers[it.key()]=it.value()->text();
 auto editors=profileEditors_;const auto count=draft_["profiles"].toArray().size();while(editors.size()<count)editors.append(QJsonValue::Null);if(profileIndex_>=0&&profileIndex_<count)editors[profileIndex_]=profileEditorState();
 QJsonObject state{{"draft",candidate()},{"numbers",numbers},{"profileEditors",editors},{"baseRevision",baseRevision_},{"observedRevision",observed_},{"dirty",dirty_},{"conflict",conflict_},{"initialProfile",initialProfile_},{"profileIndex",profile_->currentIndex()},{"section",findChild<QListWidget*>("settingsNavigation")->currentRow()},{"advanced",findChild<QToolButton*>("advancedSettings")->isChecked()},{"geometry",QJsonArray{x(),y(),width(),height()}},{"setupJobs",setupJobs_}};
 // Do not even export unclassified provider text to the resident. A content-free
 // rejection keeps the actual draft in this window until the user corrects it.
 if(auto error=validateUiState({{"schemaVersion",1},{"settings",state}}))return {{"stateError",error.code}};
 return state;
}
bool SettingsWindow::restoreViewState(const QJsonObject& state){
 if(validateUiState({{"schemaVersion",1},{"settings",state}}))return false;
 QSignalBlocker stateSignals(this);clearCredentialInputs();loading_=true;draft_=state["draft"].toObject();profileEditors_=state["profileEditors"].toArray();baseRevision_=state["baseRevision"].toString();observed_=state["observedRevision"].toString();initialProfile_=state["initialProfile"].toBool();
 populate();loading_=true;profile_->setCurrentIndex(state["profileIndex"].toInt(-1));loadProfile();const auto numbers=state["numbers"].toObject();for(auto it=numbers_.begin();it!=numbers_.end();++it)it.value()->setText(numbers[it.key()].toString());
 findChild<QToolButton*>("advancedSettings")->setChecked(state["advanced"].toBool());findChild<QListWidget*>("settingsNavigation")->setCurrentRow(state["section"].toInt());const auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());
 dirty_=state["dirty"].toBool();conflict_=state["conflict"].toBool();loading_=false;save_->setEnabled(!conflict_);setupJobs_=state["setupJobs"].toArray();updateSetupButton();if(!setupJobs_.isEmpty()){setupTimer_->start();pollSetup();}
 configurationChanged();stateSignals.unblock();emit viewStateChanged();return true;
}
void SettingsWindow::clearCredentialInputs(){if(protonPassword_)protonPassword_->clear();access_->clear();secret_->clear();token_->clear();}
void SettingsWindow::closeEvent(QCloseEvent* event){clearCredentialInputs();RetainedWindow::closeEvent(event);}
void SettingsWindow::configurationChanged(){
 refreshMetadata();if(saving_||resetting_)return;
 if(conflict_||(dirty_&&(baseRevision_!=settings_.revision()||observed_!=settings_.metadata()["observedRevision"].toString()))){conflict_=true;save_->setEnabled(false);VisualStyle::feedback(status_,"config_conflict: The configuration changed elsewhere. Your draft is preserved. Reload from file before saving.",true);emit viewStateChanged();return;}
 if(!dirty_&&(baseRevision_!=settings_.revision()||observed_!=settings_.metadata()["observedRevision"].toString()))refresh();
 auto error=settings_.metadata()["validation"].toObject()["error"].toObject();if(!error["code"].toString().isEmpty())status({error["code"].toString(),error["message"].toString(),error["details"].toObject()});
}
void SettingsWindow::beginSetup(const QString& kind,const QJsonObject& input){
 if(dirty_||conflict_||initialProfile_||profile_->currentIndex()<0){status({"save_first","Save or reload the destination before setup",{}});return;}
 const auto profileId=profileId_->text();QJsonObject params{{"profileId",profileId},{"expectedRevision",settings_.revision()}};if(kind=="storeCredentials")params["credentials"]=input;else if(kind=="createFolder"||kind=="storeSharePassword")params["input"]=input;
 if(kind=="testDestination")testAccepting_=true;
 updateSetupButton();
 ++setupAccepting_;updateSetupButton();emit viewStateChanged();
 settings_.request("setup."+kind,params,this,[this,kind,profileId](Error e,QJsonObject data){
  --setupAccepting_;if(kind=="testDestination")testAccepting_=false;
  if(e){updateSetupButton();status(e);emit viewStateChanged();return;}
  for(int i=setupJobs_.size()-1;i>=0;--i){const auto old=setupJobs_[i].toObject();if(old["kind"]==kind&&old["profileId"]==profileId)setupJobs_.removeAt(i);}
  setupJobs_.append(QJsonObject{{"setupId",data["setupId"]},{"kind",kind},{"profileId",profileId}});
  updateSetupButton();VisualStyle::feedback(status_,"Setup accepted; waiting for the resident result.");
  emit viewStateChanged();setupTimer_->start();pollSetup();
 });
}
void SettingsWindow::updateSetupButton(){
 const bool busy=setupAccepting_||!setupJobs_.isEmpty();
 findChild<QWidget*>("destinationEditor")->setEnabled(!busy);findChild<QWidget*>("credentialsEditor")->setEnabled(!busy);protonPanel_->setEnabled(!busy);profile_->setEnabled(!busy);findChild<QPushButton*>("addS3Destination")->setEnabled(!busy);findChild<QPushButton*>("removeDestination")->setEnabled(!busy);findChild<QPushButton*>("importDevelopmentS3")->setEnabled(!busy);save_->setEnabled(!busy&&!conflict_&&!saving_);
 const bool pending=std::any_of(setupJobs_.cbegin(),setupJobs_.cend(),[](const QJsonValue& value){return value.toObject()["kind"]=="testDestination";});
 findChild<QPushButton*>("testDestination")->setEnabled(!testAccepting_&&!pending);
}
void SettingsWindow::pollSetup(){
 if(setupPollActive_||setupJobs_.isEmpty()){if(setupJobs_.isEmpty())setupTimer_->stop();return;}
 setupPollActive_=true;const auto id=setupJobs_.first().toObject()["setupId"].toString();
 settings_.request("setup.inspect",{{"id",id}},this,[this,id](Error e,QJsonObject data){
  setupPollActive_=false;
  int index=0;while(index<setupJobs_.size()&&setupJobs_[index].toObject()["setupId"]!=id)++index;
  if(index==setupJobs_.size())return;
  const auto event=data["event"].toObject();
  if(!e&&event["event"]=="auth"&&!protonBrowserOpened_){const QUrl url(event["url"].toString());if(url.scheme()=="https"&&url.host()=="account.proton.me"){protonBrowserOpened_=true;QDesktopServices::openUrl(url);}}
  if(e.code=="setup_not_found"||(!e&&data["state"]!="running")){
   setupJobs_.removeAt(index);updateSetupButton();if(setupJobs_.isEmpty())setupTimer_->stop();
   if(e)e.message="This setup result is no longer available; the action was not repeated.";
   else{const auto error=data["error"].toObject();e={error["code"].toString(),error["message"].toString(),error["details"].toObject()};}
   if(e)status(e);else {
    const auto result=data["result"].toObject();const auto kind=data["kind"].toString();
    if(QStringList{"connectAccount","inspectAccount","createFolder","storeSharePassword"}.contains(kind)&&(data["profileId"]!=profileId_->text()||providerType_->currentData()!="proton-drive"||conflict_)){VisualStyle::feedback(status_,"Setup completed for the original destination. Refresh that destination before applying its result.",true);emit viewStateChanged();return;}

    if(kind=="connectAccount"||kind=="inspectAccount"){protonAccountUid_=result["accountRootUid"].toString();const auto previous=protonFolder_->currentData().toString();protonFolder_->clear();protonFolder_->addItem("Choose a folder",QString{});for(const auto& value:result["folders"].toArray()){const auto folder=value.toObject();addProtonFolder(protonFolder_,folder["uid"].toString(),folder["name"].toString());}protonFolder_->setCurrentIndex(qMax(0,protonFolder_->findData(previous)));protonAccount_->setText("Connected. Choose a folder and save settings.");dirty();}
    else if(kind=="createFolder"){protonAccountUid_=result["accountRootUid"].toString();addProtonFolder(protonFolder_,result["uid"].toString(),result["name"].toString());protonFolder_->setCurrentIndex(protonFolder_->count()-1);dirty();}
    else if(kind=="storeSharePassword"){auto profiles=draft_["profiles"].toArray();if(profileIndex_>=0&&profileIndex_<profiles.size()){auto p=profiles[profileIndex_].toObject();auto s=p["settings"].toObject();s["sharePasswordReference"]=result["reference"];p["settings"]=s;profiles[profileIndex_]=p;draft_["profiles"]=profiles;dirty();}}
    VisualStyle::success(status_,QStringList{"connectAccount","inspectAccount","createFolder","storeSharePassword"}.contains(kind)&&dirty_?"Account setup completed. Save the updated destination settings.":"Setup completed.");
   }
   emit viewStateChanged();
  }else{
   if(e)status(e);
   // Retry only the read-only inspection. Keep other accepted jobs moving too.
   if(setupJobs_.size()>1){const QJsonValue pending=setupJobs_[index];setupJobs_.removeAt(index);setupJobs_.append(pending);}
  }
 });
}
}
