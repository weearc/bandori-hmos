# Original SDK login static route

Root: `Lcom/gsc/pub/GSCPubCommon;->login(Lcom/bsgamesdk/android/callbacklistener/CallbackListener;)V`

Visited 288 method signatures, 656 direct invoke edges. No game code executed.

## Observed UI call boundaries

- `Landroid/app/Application;->getPackageManager()Landroid/content/pm/PackageManager;`
- `Landroid/app/Application;->getSharedPreferences(Ljava/lang/String;I)Landroid/content/SharedPreferences;`

## Native boundaries



## Limits

- Static invoke graph only, not executed control flow.
- Interface/virtual dispatch, reflection, registered callbacks and queued runnables may hide additional UI paths.
- A missing UI method in this bounded graph does not prove it is unused.
