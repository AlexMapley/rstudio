/*
 * VisualModeLineWrappingDialog.java
 *
 * Copyright (C) 2022 by Posit Software, PBC
 *
 * Unless you have received this program directly from Posit Software pursuant
 * to the terms of a commercial license agreement with Posit Software, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */


package org.rstudio.studio.client.workbench.views.source.editors.text.visualmode.dialogs;

import com.google.gwt.aria.client.Roles;
import com.google.gwt.core.client.GWT;
import com.google.gwt.event.logical.shared.ValueChangeHandler;
import com.google.gwt.safehtml.shared.SafeHtmlBuilder;

import org.rstudio.core.client.widget.ModalDialog;
import org.rstudio.core.client.widget.NumericValueWidget;
import org.rstudio.core.client.widget.Operation;
import org.rstudio.core.client.widget.OperationWithInput;
import org.rstudio.studio.client.common.HelpLink;
import org.rstudio.studio.client.workbench.prefs.model.UserPrefs;
import org.rstudio.studio.client.workbench.prefs.model.UserPrefsAccessor;

import com.google.gwt.user.client.ui.HTML;
import com.google.gwt.user.client.ui.Label;
import com.google.gwt.user.client.ui.RadioButton;
import com.google.gwt.user.client.ui.VerticalPanel;
import com.google.gwt.user.client.ui.Widget;
import org.rstudio.studio.client.workbench.views.source.ViewsSourceConstants;

public class VisualModeLineWrappingDialog extends ModalDialog<VisualModeLineWrappingDialog.Result>
{   
   public class Result
   {
      public Result(Action action)
      {
         this(action, null);
      }
      
      public Result(Action action, Integer column)
      {
         this.action = action;
         this.column = column;
      }
      public final Action action;
      public final Integer column;
   }
   
   public enum Action
   {
      SetFileLineWrapping,
      SetProjectLineWrapping,
      SetNothing
   }
   
   
   public VisualModeLineWrappingDialog(
      String detectedLineWrapping,
      String configuredLineWrapping,
      boolean isProjectConfig,
      boolean haveProject,
      int defaultColumnBreak,
      OperationWithInput<Result> onConfirm,
      Operation onCancel)
   {
      super(constants_.lineWrapping(),
            Roles.getDialogRole(), 
            onConfirm, 
            onCancel);
   
      
      
      mainWidget_ = new VerticalPanel();
     
      mainWidget_.addStyleName(RES.styles().confirmLineWrappingDialog());
      
      String current = isProjectConfig ? constants_.projectDefault() : constants_.globalDefault();
      
      Label mismatch = new Label(
         constants_.lineWrappingDiffersFromCurrent(current)
      );
      mainWidget_.add(mismatch);
      
      SafeHtmlBuilder builder = new SafeHtmlBuilder();
      builder.appendHtmlConstant("<ul>");
      builder.appendHtmlConstant("<li style=\"margin-bottom: 10px;\">");
      builder.appendEscaped(constants_.documentUsesBasedLineWrapping(detectedLineWrapping));
      builder.appendHtmlConstant("</li>");
      builder.appendHtmlConstant("<li style=\"margin-bottom: 3px;\">");
      if (configuredLineWrapping.equals(UserPrefsAccessor.VISUAL_MARKDOWN_EDITING_WRAP_NONE))
         builder.appendEscaped(constants_.defaultNoLineWrapping(current));
      else 
         builder.appendEscaped(constants_.defaultConfiguredBasedLineWrapping(current, configuredLineWrapping));
      builder.appendHtmlConstant("</li>");
      builder.appendHtmlConstant("</ul>");
      
      
      mainWidget_.add(new HTML(builder.toSafeHtml()));
      
     
      Label choiceLabel = new Label(constants_.selectHandleLineWrapping());
      mainWidget_.add(choiceLabel);
         
      
      chkConfigureFile_ = lineWrappingRadio( 
         constants_.useBasedLineWrapping(detectedLineWrapping)
      );
      chkConfigureFile_.setValue(true);
      mainWidget_.add(chkConfigureFile_);
      
      numFileColumn_ = createColumnInput(defaultColumnBreak);
      mainWidget_.add(numFileColumn_);
      
      chkConfigureProject_= lineWrappingRadio(
         constants_.useBasedLineWrapping(detectedLineWrapping)
      );
      numProjectColumn_ = createColumnInput(defaultColumnBreak);

      if (haveProject)
      {
         mainWidget_.add(chkConfigureProject_);
         mainWidget_.add(numProjectColumn_);
      }
      
      chkConfigureNone_ = lineWrappingRadio(
         constants_.useDefaultLinewrapping((isProjectConfig ? constants_.project() : constants_.global()))
      );
      mainWidget_.add(chkConfigureNone_);
      
      
      HelpLink lineWrappingHelp = new HelpLink(
         constants_.learnAboutVisualModeLineWrapping(),
         "visual_markdown_editing-line-wrapping",
         false
      );
      lineWrappingHelp.addStyleName(RES.styles().lineWrappingHelp());
      mainWidget_.add(lineWrappingHelp);
      
      boolean wrapColumn = detectedLineWrapping.equals(UserPrefsAccessor.VISUAL_MARKDOWN_EDITING_WRAP_COLUMN);
      ValueChangeHandler<Boolean> manageColumnInputs = (ignored) -> {
         numFileColumn_.setVisible(wrapColumn && chkConfigureFile_.getValue());
         numProjectColumn_.setVisible(wrapColumn && chkConfigureProject_.getValue());
      };
      manageColumnInputs.onValueChange(null);
      chkConfigureFile_.addValueChangeHandler(manageColumnInputs);
      chkConfigureProject_.addValueChangeHandler(manageColumnInputs);
      chkConfigureNone_.addValueChangeHandler(manageColumnInputs);
      
      
      
   }
   
  
   @Override
   protected Widget createMainWidget()
   {
      return mainWidget_;
   }
   
  
   
   @Override
   protected Result collectInput()
   {
      if (chkConfigureFile_.getValue())
         return new Result(Action.SetFileLineWrapping, Integer.parseInt(numFileColumn_.getValue()));
      else if (chkConfigureProject_.getValue())
         return new Result(Action.SetProjectLineWrapping, Integer.parseInt(numProjectColumn_.getValue()));
      else
         return new Result(Action.SetNothing);
   }


   @Override
   protected boolean validate(Result result)
   {
      if (numFileColumn_.isVisible())
         return numFileColumn_.validate();
      else if (numProjectColumn_.isVisible())
         return numProjectColumn_.validate();
      else
         return true;
   }
   
  
   private RadioButton lineWrappingRadio(String caption)
   {
      final String kRadioGroup = "DDFEDF81-87F7-45E8-B5FC-E021FC41FC69";
      RadioButton radio = new RadioButton(kRadioGroup, caption);
      radio.addStyleName(RES.styles().lineWrappingRadio());
      return radio;
   }
   
   private NumericValueWidget createColumnInput(int defaultValue)
   {
      NumericValueWidget num = new NumericValueWidget(constants_.wrapAtColumnColon(), 1, UserPrefs.MAX_WRAP_COLUMN);
      num.addStyleName(RES.styles().wrapAtColumn());
      num.setValue(Integer.toString(defaultValue));
      return num;
   }
   
   
   private VerticalPanel mainWidget_; 
   private RadioButton chkConfigureFile_;
   private NumericValueWidget numFileColumn_;
   private RadioButton chkConfigureProject_;
   private NumericValueWidget numProjectColumn_;
   private RadioButton chkConfigureNone_;
   
   
   private static VisualModeDialogsResources RES = VisualModeDialogsResources.INSTANCE;
   private static final ViewsSourceConstants constants_ = GWT.create(ViewsSourceConstants.class);
}
