/* -*- Mode: java; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is the Grendel mail/news client.
 *
 * The Initial Developer of the Original Code is Netscape Communications
 * Corporation.  Portions created by Netscape are
 * Copyright (C) 1997 Netscape Communications Corporation. All
 * Rights Reserved.
 *
 * Contributor(s): 
 *
 * Created: Terry Weissman <terry@netscape.com>, 27 Oct 1997.
 */

package grendel.storage;

import javax.mail.Message;

/** If you want to call any of the MessageExtra methods on a Message, you
  should get the MessageExtra object using this call.  Some Message objects
  will implement that interface directly; the rest need to have them
  painfully computed. */

public class MessageExtraFactory {
  static public MessageExtra Get(Message m) {
    if (m instanceof MessageExtra) return (MessageExtra) m;
    return new MessageExtraWrapper(m);
  }
}

